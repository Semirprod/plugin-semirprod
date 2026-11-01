#pragma once

// ---------------------------------------------------------------------------
// Minimal, dependency-free RFC 6455 framing used by both the outbound WebSocket
// client (LiveClient, over QSslSocket) and the local WebSocket server
// (WebUiServer, over QTcpServer).
//
// WHY THIS EXISTS: the plugin used to link Qt6::WebSockets. That module is NOT
// shipped by OBS, so we bundled Qt6WebSockets.dll alongside the plugin — and
// QtWebSockets links a PRIVATE Qt Core symbol (QObjectPrivate::QObjectPrivate)
// whose signature changes across Qt minor versions. The moment OBS bumped its
// bundled Qt (6.8.3 -> 6.11.1) the bundled 6.8.3 Qt6WebSockets.dll could no
// longer resolve that symbol, the plugin failed to load (LoadLibrary error 127)
// and every client broke. Building the framing ourselves on top of QtNetwork's
// public QSslSocket / QTcpSocket API — which OBS DOES ship and which Qt keeps
// binary-compatible within Qt6 — removes that version lock for good.
//
// Only the pieces the plugin needs are implemented: single logical text/binary
// messages (with fragmentation reassembly on receive), ping/pong, and close.
// ---------------------------------------------------------------------------

#include <QByteArray>
#include <QString>

#include <functional>

namespace semirprod::ws {

// RFC 6455 opcodes.
enum Opcode : quint8 {
  OpContinuation = 0x0,
  OpText = 0x1,
  OpBinary = 0x2,
  OpClose = 0x8,
  OpPing = 0x9,
  OpPong = 0xA,
};

// Hard ceiling on a single inbound frame's declared payload length. Guards
// against a malicious/buggy peer announcing a huge length and forcing us to
// buffer until OOM.
//
// Abaissé de 64 MiB à 1 MiB : ces canaux ne transportent que du JSON de
// contrôle de quelques kio. À 64 MiB, une seule trame coûtait déjà ~256 Mo de
// pic (buffer + copie + conversion UTF-16) sur le thread principal d'OBS,
// pendant un direct.
constexpr quint64 kMaxInboundPayload = 1ull * 1024 * 1024;

// Plafond CUMULÉ d'un message fragmenté. Le plafond ci-dessus ne s'applique
// qu'à UNE trame : une suite illimitée de trames de continuation faisait
// croître le tampon de réassemblage sans aucune borne, jusqu'à l'épuisement
// mémoire du processus OBS. Le budget de commandes du serveur local ne protège
// pas non plus, puisqu'il compte les messages complets et non les trames.
constexpr quint64 kMaxInboundMessage = 4ull * 1024 * 1024;

// RFC 6455 §5.5 : une trame de contrôle porte au plus 125 octets de charge
// utile et ne peut pas être fragmentée.
constexpr int kMaxControlPayload = 125;

// Build a complete frame. `mask` MUST be true for client->server frames
// (RFC 6455 §5.3) and MUST be false for server->client frames. When masking,
// a fresh 4-byte key from the system CSPRNG is used per frame.
QByteArray encodeFrame(quint8 opcode, const QByteArray& payload, bool mask);

// Server-side Sec-WebSocket-Accept for a given client Sec-WebSocket-Key, and
// the client-side check use the same helper: base64(SHA1(key + GUID)).
QString acceptFor(const QString& secWebSocketKey);

// 16 random bytes, base64 — a fresh client Sec-WebSocket-Key per handshake.
QString makeClientKey();

// Incremental frame parser. Feed raw socket bytes via append(); it invokes the
// callbacks as complete frames arrive. Not thread-safe — drive it from one
// thread (the Qt event-loop thread that owns the socket).
class Parser {
 public:
  std::function<void(const QString&)> onText;
  std::function<void(const QByteArray&)> onPing;
  std::function<void(const QByteArray&)> onPong;
  std::function<void(quint16 code, const QString& reason)> onClose;
  // Fatal protocol violation — the owner should drop the connection.
  std::function<void(const QString&)> onError;

  void append(const QByteArray& data);
  void reset();

 private:
  // Parse exactly one frame from buf_. Returns false when more bytes are
  // needed (buf_ left untouched) or on fatal error (buf_ cleared).
  bool parseOne();
  void dispatch(bool fin, quint8 opcode, const QByteArray& payload);

  QByteArray buf_;
  int fragmentOpcode_ = -1;  // -1 == not mid-fragment
  QByteArray fragmentPayload_;
  bool errored_ = false;
};

}  // namespace semirprod::ws
