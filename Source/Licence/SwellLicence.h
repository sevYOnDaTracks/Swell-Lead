#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// Licences Swell : module commun à tous les plugins.
// Pour l'ajouter à un autre plugin : copier le dossier Source/Licence, écrire son Source/LicenceConfig.h,
// ajouter SwellLicence.cpp et Monocypher au CMakeLists.txt (voir celui de Log Drums).
//
// Fonctionnement :
//  - Le client colle sa clé (SWELL-XXXXX-...). Le plugin l'envoie une fois à swell-audio.fr/api/licenses/activate
//    avec un identifiant de l'ordinateur, et reçoit un fichier de licence signé en Ed25519.
//  - Le fichier est enregistré dans « Swell Audio/Licences » et vérifié hors ligne à chaque chargement avec la clé
//    publique intégrée : sans la clé privée du serveur, impossible d'en fabriquer un.
//  - Démo : une clé d'essai donne un fichier avec une date de fin (expiresAt, signée). Passé cette date, silence
//    et écran « Essai terminé ». Le plugin retient la date la plus récente vue : reculer l'horloge n'y change rien.
//    À chaque chargement d'un essai, il redemande le fichier : après achat, la même clé devient définitive.
//  - Tous les 30 jours, si internet répond, le plugin redemande le fichier. Si la clé a été désactivée sur le site
//    (ou l'ordinateur retiré sans place libre), la licence est retirée. Sans réseau, rien ne change.

namespace swell
{
class Licence : public juce::ChangeBroadcaster
{
public:
    struct Info
    {
        juce::String key, product, machine, email, name;
        juce::Time issuedAt;
        juce::Time expiresAt; // essai seulement (sinon 0)
        bool isTrial() const noexcept { return expiresAt.toMilliseconds() > 0; }
    };

    Licence();  // charge et vérifie le fichier enregistré (rapide : aucun accès réseau ici)
    ~Licence() override;

    // Lu par le thread audio : sans licence, le plugin ne sort que du silence.
    bool isUnlocked() const noexcept
    {
        if (testUnlock.load()) return true;
        const auto expiry = expiryMs.load();
        return unlocked.load() && (expiry == 0 || now() < expiry);
    }
    bool isTrialExpired() const noexcept { const auto e = expiryMs.load(); return unlocked.load() && e != 0 && now() >= e; }
    int trialDaysLeft() const noexcept;  // 0 si ce n'est pas un essai
    Info info() const;
    bool isBusy() const noexcept { return busy.load(); }

    // Activation en arrière-plan ; 'done' est appelé sur le thread des messages avec un message lisible.
    void activate (const juce::String& key, std::function<void (bool ok, juce::String message)> done);
    // Retire la licence de cet ordinateur et libère sa place sur le site.
    void deactivate();

    // Vérification d'un fichier de licence (signature, plugin et ordinateur). Publique pour les tests.
    static bool verify (const juce::String& file, const juce::String& product, const juce::String& machine, Info& out);
    static juce::String machineId();
    static juce::String machineName();
    static juce::File licenceFile();
    static juce::String normaliseKey (const juce::String& raw);  // vide si le format est mauvais

    // Tests et outils hors ligne (rendu audio, captures) : débloque sans fichier.
    static void unlockForTesting() noexcept { testUnlock.store (true); }

private:
    struct Response { int status = 0; juce::var body; };
    static Response post (const juce::String& path, const juce::var& body);
    static juce::String server();
    // Heure « sûre » : jamais avant la plus récente vue par le plugin (horloge reculée).
    juce::int64 now() const noexcept { return juce::jmax (juce::Time::currentTimeMillis(), latestSeenMs.load()); }
    void rememberClock();
    void load();
    void recheckIfStale();
    void store (const juce::String& file, const Info&);
    void forget();

    mutable juce::CriticalSection lock;
    Info current;
    std::atomic<bool> unlocked { false }, busy { false };
    std::atomic<juce::int64> expiryMs { 0 }, latestSeenMs { 0 };
    static inline std::atomic<bool> testUnlock { false };
    juce::ThreadPool pool { juce::ThreadPoolOptions().withNumberOfThreads (1).withThreadName ("Swell licence") };

    JUCE_DECLARE_WEAK_REFERENCEABLE (Licence)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Licence)
};

// Écran d'activation, posé par-dessus l'interface tant que le plugin n'est pas activé.
class LicencePanel : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    struct Style
    {
        juce::Colour background, card, ink, muted, accent, error;
    };

    explicit LicencePanel (Style);
    ~LicencePanel() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override { changeListenerCallback (nullptr); } // un essai peut finir pendant la session
    void submit();
    juce::Rectangle<int> cardBounds() const;

    juce::SharedResourcePointer<Licence> licence;
    Style style;
    juce::TextEditor keyField;
    // Style propre au bouton : le LookAndFeel du plugin ne doit pas le rendre illisible.
    juce::LookAndFeel_V4 buttonLook;
    juce::TextButton activateButton;
    juce::HyperlinkButton accountLink, buyLink;
    juce::String status;
    bool statusIsError = false;
};

// Écran d'activation posé par-dessus n'importe quelle fenêtre de plugin, en une ligne :
//     return swell::LicenceGuard::attach (new MonEditeur (*this));   // dans createEditor()
// ou bien, comme membre de l'éditeur :  swell::LicenceGuard licenceGuard { *this };
// Il suit la taille de la fenêtre (réduit si elle est petite) et reste au premier plan.
class LicenceGuard : private juce::ComponentListener, private juce::AsyncUpdater
{
public:
    explicit LicenceGuard (juce::Component& editor, LicencePanel::Style style = defaultStyle());
    ~LicenceGuard() override;
    static LicencePanel::Style defaultStyle();

    // Accroche une garde à l'éditeur ; elle disparaît avec lui.
    template <typename Editor>
    static Editor* attach (Editor* editor)
    {
        (new LicenceGuard (*editor))->ownedByEditor = true;
        return editor;
    }

private:
    void componentMovedOrResized (juce::Component&, bool, bool) override { layout(); }
    void componentChildrenChanged (juce::Component&) override { triggerAsyncUpdate(); }
    void componentBeingDeleted (juce::Component&) override { if (ownedByEditor) delete this; }
    void handleAsyncUpdate() override { if (panel.isVisible()) panel.toFront (false); }
    void layout();

    juce::Component& editor;
    LicencePanel panel;
    bool ownedByEditor = false;
};

// « Licence : Prénom Nom », ou « Essai : 5 jours restants · Acheter » ; un clic ouvre le menu.
class LicenceBadge : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    LicenceBadge (juce::Colour text, juce::Colour hover, juce::Justification align = juce::Justification::centredRight);
    ~LicenceBadge() override;
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    void timerCallback() override { repaint(); }
    juce::SharedResourcePointer<Licence> licence;
    juce::Colour text, hover;
    juce::Justification align;
};
} // namespace swell
