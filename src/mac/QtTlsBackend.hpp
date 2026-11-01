#pragma once

// macOS uniquement : rendre le TLS de Qt utilisable dans OBS.
//
// OBS.app ne livre AUCUN backend TLS Qt. Son `Contents/PlugIns/` contient
// platforms, styles, imageformats et iconengines — pas de dossier `tls`. Ce
// n'est pas un oubli : OBS fait son HTTPS avec libcurl/mbedTLS et
// obs-websocket avec websocketpp, donc personne cote OBS n'a jamais eu besoin
// de QSslSocket. Nous si, pour la totalite de nos echanges (API, appairage,
// wss://, verification de mise a jour).
//
// Sans backend, Qt ne rate pas la verification du certificat : il refuse
// d'ouvrir la connexion, et QNetworkReply rend l'erreur 99
// (QNetworkReply::SslHandshakeFailedError) avec « TLS initialization failed ».
// Le plugin se charge, l'interface s'affiche, et absolument aucune requete ne
// part — le pire mode de panne possible pour un diagnostic a distance.
//
// On embarque donc le backend SecureTransport d'obs-deps dans notre propre
// bundle (Contents/PlugIns/tls/) et on declare ce dossier a Qt avant la
// premiere socket. Voir CMakeLists.txt, bloc « BACKEND TLS » pour la copie.
namespace semirprod {

// Idempotent, a appeler le plus tot possible dans obs_module_load.
// No-op ailleurs que sur macOS.
void ensureQtTlsBackend();

} // namespace semirprod
