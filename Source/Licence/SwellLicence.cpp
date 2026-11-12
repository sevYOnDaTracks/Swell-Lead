#include "SwellLicence.h"
#include "../LicenceConfig.h"
#include <juce_cryptography/juce_cryptography.h>
#include <monocypher-ed25519.h>
#include <cmath>

namespace swell
{
namespace
{
constexpr const char* defaultServer = "https://swell-audio.fr";
const auto recheckAfter = juce::RelativeTime::days (30);
const char* keyLetters = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

juce::String text (const char* utf8) { return juce::String::fromUTF8 (utf8); }

juce::MemoryBlock fromBase64Url (juce::String s)
{
    s = s.replaceCharacter ('-', '+').replaceCharacter ('_', '/');
    while (s.length() % 4 != 0) s << "=";
    juce::MemoryOutputStream out;
    if (! juce::Base64::convertFromBase64 (out, s)) return {};
    return out.getMemoryBlock();
}

juce::String messageFrom (const juce::var& body, const char* fallback)
{
    const auto m = body.getProperty ("message", {}).toString();
    return m.isNotEmpty() ? m : text (fallback);
}
}

// ---------------------------------------------------------------- Licence

Licence::Licence() { load(); }

Licence::~Licence() { pool.removeAllJobs (true, 20000); }

Licence::Info Licence::info() const
{
    const juce::ScopedLock sl (lock);
    return current;
}

juce::String Licence::server()
{
    // Pour tester avec le site en local : SWELL_LICENCE_SERVER=http://localhost:3000
    // Sans danger : un faux serveur ne peut pas signer de licence valable.
    const auto custom = juce::SystemStats::getEnvironmentVariable ("SWELL_LICENCE_SERVER", {});
    return custom.isNotEmpty() ? custom.trimCharactersAtEnd ("/") : juce::String (defaultServer);
}

juce::String Licence::machineId()
{
    auto id = juce::SystemStats::getUniqueDeviceID();
    if (id.isEmpty()) id = juce::SystemStats::getComputerName() + juce::SystemStats::getLogonName();
    // Haché : le site ne voit jamais l'identifiant brut de la machine.
    return juce::SHA256 ((id + "|swell-licence").toUTF8()).toHexString().substring (0, 32);
}

juce::String Licence::machineName()
{
    auto name = juce::SystemStats::getComputerName().trim();
    return name.isNotEmpty() ? name.substring (0, 64) : juce::String ("Ordinateur");
}

juce::File Licence::licenceFile()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return base.getChildFile ("Swell Audio").getChildFile ("Licences").getChildFile (juce::String (licence_config::product) + ".swell-licence");
}

juce::String Licence::normaliseKey (const juce::String& raw)
{
    auto chars = raw.toUpperCase().retainCharacters ("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    if (chars.startsWith ("SWELL")) chars = chars.substring (5);
    if (chars.length() != 20 || ! chars.containsOnly (keyLetters)) return {};
    return "SWELL-" + chars.substring (0, 5) + "-" + chars.substring (5, 10) + "-" + chars.substring (10, 15) + "-" + chars.substring (15);
}

bool Licence::verify (const juce::String& file, const juce::String& product, const juce::String& machine, Info& out)
{
    const auto trimmed = file.trim();
    const int dot = trimmed.indexOfChar ('.');
    if (dot <= 0) return false;
    const auto body = trimmed.substring (0, dot);
    const auto signature = fromBase64Url (trimmed.substring (dot + 1));
    if (signature.getSize() != 64) return false;

    // La signature porte sur les octets exacts de la première partie, telle qu'elle est écrite.
    const auto bodyUtf8 = body.toStdString();
    if (crypto_ed25519_check (static_cast<const uint8_t*> (signature.getData()), licence_config::publicKey,
                              reinterpret_cast<const uint8_t*> (bodyUtf8.data()), bodyUtf8.size()) != 0)
        return false;

    const auto json = fromBase64Url (body);
    const auto payload = juce::JSON::parse (juce::String::fromUTF8 (static_cast<const char*> (json.getData()), (int) json.getSize()));
    // v1 : licence définitive. v2 : essai, avec une date de fin obligatoire.
    const int version = payload.isObject() ? (int) payload.getProperty ("v", 0) : 0;
    if (version != 1 && version != 2) return false;

    Info info;
    info.key = payload.getProperty ("key", {}).toString();
    info.product = payload.getProperty ("product", {}).toString();
    info.machine = payload.getProperty ("machine", {}).toString();
    info.email = payload.getProperty ("email", {}).toString();
    info.name = payload.getProperty ("name", {}).toString();
    info.issuedAt = juce::Time::fromISO8601 (payload.getProperty ("issuedAt", {}).toString());
    if (version == 2)
    {
        info.expiresAt = juce::Time::fromISO8601 (payload.getProperty ("expiresAt", {}).toString());
        if (! info.isTrial()) return false;
    }
    if (info.product != product || info.machine != machine || info.key.isEmpty()) return false;
    out = info;
    return true;
}

int Licence::trialDaysLeft() const noexcept
{
    const auto expiry = expiryMs.load();
    if (expiry == 0) return 0;
    return juce::jmax (0, (int) std::ceil ((double) (expiry - now()) / (24.0 * 3600 * 1000)));
}

void Licence::rememberClock()
{
    // Date la plus récente vue, gardée à côté des licences (commune à tous les plugins Swell).
    const auto f = licenceFile().getSiblingFile (".horloge");
    const auto saved = f.loadFileAsString().trim().getLargeIntValue();
    const auto latest = juce::jmax (saved, juce::Time::currentTimeMillis());
    latestSeenMs.store (latest);
    if (latest != saved)
    {
        f.getParentDirectory().createDirectory();
        f.replaceWithText (juce::String (latest));
    }
}

void Licence::load()
{
    rememberClock();
    const auto file = licenceFile();
    Info info;
    if (file.existsAsFile() && verify (file.loadFileAsString(), licence_config::product, machineId(), info))
    {
        {
            const juce::ScopedLock sl (lock);
            current = info;
        }
        expiryMs.store (info.expiresAt.toMilliseconds());
        unlocked.store (true);
        recheckIfStale();
    }
}

void Licence::store (const juce::String& file, const Info& info)
{
    const auto f = licenceFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText (file);
    {
        const juce::ScopedLock sl (lock);
        current = info;
    }
    expiryMs.store (info.expiresAt.toMilliseconds());
    unlocked.store (true);
    sendChangeMessage();
}

void Licence::forget()
{
    licenceFile().deleteFile();
    {
        const juce::ScopedLock sl (lock);
        current = {};
    }
    expiryMs.store (0);
    unlocked.store (false);
    sendChangeMessage();
}

Licence::Response Licence::post (const juce::String& path, const juce::var& body)
{
    Response r;
    const auto url = juce::URL (server() + path).withPOSTData (juce::JSON::toString (body, true));
    auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                                             .withExtraHeaders ("Content-Type: application/json\r\nAccept: application/json")
                                             .withConnectionTimeoutMs (10000)
                                             .withStatusCode (&r.status));
    if (stream == nullptr) { r.status = 0; return r; }
    r.body = juce::JSON::parse (stream->readEntireStreamAsString());
    return r;
}

void Licence::activate (const juce::String& rawKey, std::function<void (bool, juce::String)> done)
{
    const auto key = normaliseKey (rawKey);
    if (key.isEmpty())
    {
        done (false, text ("Cette clé n'a pas le bon format : SWELL-XXXXX-XXXXX-XXXXX-XXXXX."));
        return;
    }
    if (busy.exchange (true)) return;

    juce::WeakReference<Licence> weak (this);
    pool.addJob ([this, key, weak, done = std::move (done)]
    {
        auto* request = new juce::DynamicObject();
        request->setProperty ("key", key);
        request->setProperty ("product", licence_config::product);
        request->setProperty ("machineId", machineId());
        request->setProperty ("machineName", machineName());
        const auto r = post ("/api/licenses/activate", juce::var (request));

        bool ok = false;
        juce::String message;
        Info info;
        if (r.status == 200 && verify (r.body.getProperty ("license", {}).toString(), licence_config::product, machineId(), info))
        {
            store (r.body.getProperty ("license", {}).toString(), info);
            ok = true;
            message = info.isTrial()
                        ? text ("Essai activé : ") + juce::String (trialDaysLeft()) + text (" jours pour découvrir Swell ") + licence_config::productName + "."
                        : text ("Merci ") + info.name + text (" ! Swell ") + licence_config::productName + text (" est activé.");
        }
        else if (r.status == 0)
            message = text ("Impossible de joindre swell-audio.fr. Vérifie ta connexion internet et réessaie.");
        else if (r.status == 200)
            message = text ("La réponse du serveur n'est pas valable. Mets le plugin à jour ou contacte Swell.");
        else
            message = messageFrom (r.body, "L'activation a échoué. Réessaie dans un moment.");

        busy.store (false);
        juce::MessageManager::callAsync ([weak, done, ok, message]
        {
            if (weak != nullptr) done (ok, message);
        });
    });
}

void Licence::deactivate()
{
    const auto key = info().key;
    forget();
    if (key.isEmpty()) return;
    pool.addJob ([key]
    {
        auto* request = new juce::DynamicObject();
        request->setProperty ("key", key);
        request->setProperty ("machineId", machineId());
        post ("/api/licenses/deactivate", juce::var (request)); // sans réseau, la place se libère depuis le site
    });
}

void Licence::recheckIfStale()
{
    const auto info = this->info();
    // Un essai est revérifié à chaque chargement : s'il a été acheté, la clé revient définitive.
    if (! info.isTrial() && juce::Time::getCurrentTime() - info.issuedAt < recheckAfter) return;

    pool.addJob ([this, info]
    {
        auto* request = new juce::DynamicObject();
        request->setProperty ("key", info.key);
        request->setProperty ("product", licence_config::product);
        request->setProperty ("machineId", machineId());
        request->setProperty ("machineName", machineName());
        const auto r = post ("/api/licenses/activate", juce::var (request));

        Info fresh;
        if (r.status == 200 && verify (r.body.getProperty ("license", {}).toString(), licence_config::product, machineId(), fresh))
            store (r.body.getProperty ("license", {}).toString(), fresh);
        else if (r.status == 403 && r.body.getProperty ("error", {}).toString() == "trial_expired")
            return; // essai fini et pas encore acheté : on garde le fichier pour afficher « Essai terminé »
        else if (r.status == 403 || r.status == 404 || r.status == 409)
            forget(); // clé désactivée, inconnue, ou ordinateur retiré sans place libre
        // Pas de réseau ou serveur en panne : la licence reste valable, on réessaiera au prochain chargement.
    });
}

// ---------------------------------------------------------------- Écran d'activation

LicencePanel::LicencePanel (Style s) : style (s)
{
    setInterceptsMouseClicks (true, true);
    licence->addChangeListener (this);

    keyField.setTextToShowWhenEmpty ("SWELL-XXXXX-XXXXX-XXXXX-XXXXX", style.muted);
    keyField.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 17.f, juce::Font::plain));
    keyField.setJustification (juce::Justification::centred);
    keyField.setInputRestrictions (40);
    keyField.setColour (juce::TextEditor::backgroundColourId, style.background);
    keyField.setColour (juce::TextEditor::textColourId, style.ink);
    keyField.setColour (juce::TextEditor::outlineColourId, style.muted.withAlpha (.4f));
    keyField.setColour (juce::TextEditor::focusedOutlineColourId, style.accent);
    keyField.setColour (juce::CaretComponent::caretColourId, style.accent);
    keyField.onReturnKey = [this] { submit(); };
    addAndMakeVisible (keyField);

    activateButton.setButtonText ("Activer");
    activateButton.setLookAndFeel (&buttonLook);
    activateButton.setColour (juce::TextButton::buttonColourId, style.accent);
    activateButton.setColour (juce::TextButton::textColourOffId, style.background);
    activateButton.onClick = [this] { submit(); };
    addAndMakeVisible (activateButton);

    accountLink.setButtonText (text ("Où trouver ma clé ?"));
    accountLink.setURL (juce::URL (juce::String (defaultServer) + "/compte#achats"));
    buyLink.setButtonText (text ("Acheter Swell ") + licence_config::productName);
    buyLink.setURL (juce::URL (juce::String (defaultServer) + "/boutique/" + licence_config::product));
    for (auto* link : { &accountLink, &buyLink })
    {
        link->setColour (juce::HyperlinkButton::textColourId, style.muted);
        link->setFont (juce::FontOptions (13.f), false);
        addAndMakeVisible (link);
    }
    setVisible (! licence->isUnlocked());
    if (licence->isTrialExpired()) keyField.setText (licence->info().key, false);
    startTimer (30000);
}

LicencePanel::~LicencePanel()
{
    activateButton.setLookAndFeel (nullptr);
    licence->removeChangeListener (this);
}

juce::Rectangle<int> LicencePanel::cardBounds() const { return getLocalBounds().withSizeKeepingCentre (500, 330); }

void LicencePanel::paint (juce::Graphics& g)
{
    g.fillAll (style.background.withAlpha (.9f));
    const auto card = cardBounds().toFloat();
    g.setColour (style.card);
    g.fillRoundedRectangle (card, 14);
    g.setColour (style.accent);
    g.fillRoundedRectangle (card.withHeight (3).reduced (24, 0), 1.5f);

    auto area = card.reduced (36, 30);
    const bool expired = licence->isTrialExpired();
    g.setColour (style.muted);
    g.setFont (juce::FontOptions (10.f).withKerningFactor (.25f));
    g.drawText (expired ? "ESSAI" : "ACTIVATION", area.removeFromTop (14), juce::Justification::centredLeft);
    area.removeFromTop (8);
    g.setColour (style.ink);
    g.setFont (juce::FontOptions (28.f));
    g.drawText (expired ? text ("Ton essai est terminé") : text ("Active Swell ") + licence_config::productName,
                area.removeFromTop (36), juce::Justification::centredLeft);
    area.removeFromTop (6);
    g.setColour (style.muted);
    g.setFont (juce::FontOptions (14.f));
    g.drawFittedText (expired ? text ("Achète Swell ") + licence_config::productName
                                    + text (" sur swell-audio.fr : ta clé d'essai devient définitive. Clique ensuite sur Activer.")
                              : text ("Colle la clé de licence reçue par email ou visible dans ton espace sur swell-audio.fr. "
                                      "Internet est nécessaire une seule fois."),
                      area.removeFromTop (40).toNearestInt(), juce::Justification::topLeft, 2, 1.f);

    if (status.isNotEmpty())
    {
        g.setColour (statusIsError ? style.error : style.ink);
        g.setFont (juce::FontOptions (13.f));
        g.drawFittedText (status, card.toNearestInt().reduced (36, 0).withTop ((int) card.getY() + 228).withHeight (36),
                          juce::Justification::centredLeft, 2, 1.f);
    }
}

void LicencePanel::resized()
{
    const auto card = cardBounds().reduced (36, 0);
    keyField.setBounds (card.getX(), card.getY() + 170, card.getWidth() - 130, 44);
    activateButton.setBounds (card.getRight() - 118, card.getY() + 170, 118, 44);
    accountLink.setBounds (card.getX() - 6, card.getBottom() - 54, 170, 24);
    buyLink.setBounds (card.getRight() - 230, card.getBottom() - 54, 236, 24);
    accountLink.setJustificationType (juce::Justification::centredLeft);
    buyLink.setJustificationType (juce::Justification::centredRight);
}

void LicencePanel::submit()
{
    if (licence->isBusy()) return;
    status = text ("Activation en cours…");
    statusIsError = false;
    activateButton.setEnabled (false);
    repaint();

    juce::Component::SafePointer<LicencePanel> safe (this);
    licence->activate (keyField.getText(), [safe] (bool ok, juce::String message)
    {
        if (safe == nullptr) return;
        safe->status = message;
        safe->statusIsError = ! ok;
        safe->activateButton.setEnabled (true);
        safe->repaint();
    });
    if (! licence->isBusy()) activateButton.setEnabled (true); // mauvais format : réponse immédiate
}

void LicencePanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const bool show = ! licence->isUnlocked();
    if (show && ! isVisible())
    {
        status.clear();
        keyField.setText (licence->isTrialExpired() ? licence->info().key : juce::String(), false);
    }
    if (show != isVisible()) setVisible (show);
    if (show) { toFront (false); repaint(); }
}

// ---------------------------------------------------------------- Garde (écran par-dessus n'importe quel éditeur)

LicencePanel::Style LicenceGuard::defaultStyle()
{
    return { juce::Colour (0xff0b0b0c), juce::Colour (0xff141416), juce::Colour (0xffededee), juce::Colour (0xff8e8e94),
             juce::Colour (0xffff8a3d), juce::Colour (0xffff6b5b) };
}

LicenceGuard::LicenceGuard (juce::Component& e, LicencePanel::Style style) : editor (e), panel (style)
{
    editor.addChildComponent (panel); // le panneau s'affiche de lui-même tant qu'il n'y a pas de licence valable
    editor.addComponentListener (this);
    layout();
}

LicenceGuard::~LicenceGuard()
{
    cancelPendingUpdate();
    editor.removeComponentListener (this);
    editor.removeChildComponent (&panel);
}

void LicenceGuard::layout()
{
    const int w = editor.getWidth(), h = editor.getHeight();
    if (w <= 0 || h <= 0) return;
    // La carte fait 500 x 330 : dans une petite fenêtre, tout le panneau est réduit pour rester lisible.
    const float scale = juce::jmin (1.f, (float) w / 540.f, (float) h / 370.f);
    panel.setTransform (juce::AffineTransform::scale (scale));
    panel.setBounds (0, 0, juce::roundToInt ((float) w / scale), juce::roundToInt ((float) h / scale));
    if (panel.isVisible()) panel.toFront (false);
}

// ---------------------------------------------------------------- Badge

LicenceBadge::LicenceBadge (juce::Colour t, juce::Colour h, juce::Justification a) : text (t), hover (h), align (a)
{
    licence->addChangeListener (this);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    startTimer (60000); // jours restants de l'essai
}

LicenceBadge::~LicenceBadge() { licence->removeChangeListener (this); }

void LicenceBadge::paint (juce::Graphics& g)
{
    if (! licence->isUnlocked()) return;
    const auto info = licence->info();
    g.setFont (juce::FontOptions (9.f));
    if (info.isTrial())
    {
        const int days = licence->trialDaysLeft();
        g.setColour (isMouseOver() ? text : hover);
        g.drawText (swell::text ("Essai : ") + juce::String (days) + (days > 1 ? swell::text (" jours restants") : swell::text (" jour restant"))
                        + swell::text ("  ·  Acheter"),
                    getLocalBounds(), align);
        return;
    }
    g.setColour (isMouseOver() ? hover : text);
    g.drawText (swell::text ("Licence : ") + (info.name.isNotEmpty() ? info.name : info.email), getLocalBounds(), align);
}

void LicenceBadge::mouseUp (const juce::MouseEvent&)
{
    if (! licence->isUnlocked()) return;
    const auto info = licence->info();
    juce::PopupMenu menu;
    menu.addSectionHeader (info.email);
    if (info.isTrial()) menu.addItem (2, swell::text ("Acheter Swell ") + licence_config::productName);
    menu.addItem (1, swell::text ("Désactiver sur cet ordinateur"));
    juce::Component::SafePointer<LicenceBadge> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe] (int choice)
    {
        if (safe == nullptr) return;
        if (choice == 1) safe->licence->deactivate();
        if (choice == 2) juce::URL (juce::String (defaultServer) + "/boutique/" + licence_config::product).launchInDefaultBrowser();
    });
}
} // namespace swell
