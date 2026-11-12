#pragma once
#include <cstdint>

// Réglages de licence propres à ce plugin (le reste est dans Source/Licence, commun à tous les plugins Swell).
namespace swell::licence_config
{
// Doit être exactement le slug du produit dans la boutique (swell-audio.fr/boutique/<slug>).
inline constexpr const char* product = "swell-lead";
inline constexpr const char* productName = "Lead";

// Clé publique Ed25519 des licences Swell, la même pour tous les plugins.
// Sa clé privée est LICENSE_PRIVATE_KEY sur le serveur : ne jamais la mettre ici.
inline constexpr uint8_t publicKey[32] { 0x70, 0x8b, 0xfd, 0x4f, 0xa6, 0x8e, 0x9e, 0x21, 0x4c, 0xb8, 0x1c, 0x7e, 0x0c, 0x94, 0xbf, 0x5f,
                                         0x4c, 0x15, 0x49, 0x1f, 0xa5, 0x07, 0xc4, 0xa9, 0x93, 0x8e, 0x59, 0x1d, 0x4e, 0x2d, 0xc5, 0xb9 };
}
