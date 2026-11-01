#include "WsFraming.hpp"

#include <QCryptographicHash>
#include <QRandomGenerator>

#include <cstring>

namespace semirprod::ws {

namespace {
// RFC 6455 §1.3 magic GUID appended to the client key before hashing.
constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
}  // namespace

QByteArray encodeFrame(quint8 opcode, const QByteArray& payload, bool mask) {
  QByteArray out;
  // FIN=1 (we never fragment our own sends) + opcode.
  out.append(static_cast<char>(0x80 | (opcode & 0x0F)));

  const quint64 len = static_cast<quint64>(payload.size());
  const quint8 maskBit = mask ? 0x80 : 0x00;
  if (len <= 125) {
    out.append(static_cast<char>(maskBit | static_cast<quint8>(len)));
  } else if (len <= 0xFFFF) {
    out.append(static_cast<char>(maskBit | 126));
    out.append(static_cast<char>((len >> 8) & 0xFF));
    out.append(static_cast<char>(len & 0xFF));
  } else {
    out.append(static_cast<char>(maskBit | 127));
    for (int i = 7; i >= 0; --i) {
      out.append(static_cast<char>((len >> (8 * i)) & 0xFF));
    }
  }

  if (mask) {
    quint8 key[4];
    const quint32 r = QRandomGenerator::system()->generate();
    std::memcpy(key, &r, 4);
    out.append(reinterpret_cast<const char*>(key), 4);
    QByteArray masked = payload;
    for (int i = 0; i < masked.size(); ++i) {
      masked[i] = static_cast<char>(static_cast<quint8>(masked[i]) ^ key[i & 3]);
    }
    out.append(masked);
  } else {
    out.append(payload);
  }
  return out;
}

QString acceptFor(const QString& secWebSocketKey) {
  QByteArray data = secWebSocketKey.toUtf8();
  data.append(kGuid);
  const QByteArray digest = QCryptographicHash::hash(data, QCryptographicHash::Sha1);
  return QString::fromLatin1(digest.toBase64());
}

QString makeClientKey() {
  quint8 raw[16];
  for (int i = 0; i < 16; i += 4) {
    const quint32 r = QRandomGenerator::system()->generate();
    std::memcpy(raw + i, &r, 4);
  }
  return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(raw), 16).toBase64());
}

void Parser::reset() {
  buf_.clear();
  fragmentOpcode_ = -1;
  fragmentPayload_.clear();
  errored_ = false;
}

void Parser::append(const QByteArray& data) {
  if (errored_) return;
  buf_.append(data);
  while (!errored_ && parseOne()) {
    // keep draining complete frames
  }
}

bool Parser::parseOne() {
  if (buf_.size() < 2) return false;

  const quint8 b0 = static_cast<quint8>(buf_[0]);
  const quint8 b1 = static_cast<quint8>(buf_[1]);
  const bool fin = (b0 & 0x80) != 0;
  const quint8 opcode = b0 & 0x0F;
  const bool masked = (b1 & 0x80) != 0;
  quint64 len = b1 & 0x7F;
  int pos = 2;

  if (len == 126) {
    if (buf_.size() < pos + 2) return false;
    len = (static_cast<quint64>(static_cast<quint8>(buf_[pos])) << 8) |
          static_cast<quint8>(buf_[pos + 1]);
    pos += 2;
  } else if (len == 127) {
    if (buf_.size() < pos + 8) return false;
    len = 0;
    for (int i = 0; i < 8; ++i) {
      len = (len << 8) | static_cast<quint8>(buf_[pos + i]);
    }
    pos += 8;
  }

  if (len > kMaxInboundPayload) {
    errored_ = true;
    buf_.clear();
    if (onError) onError(QStringLiteral("inbound frame exceeds size cap"));
    return false;
  }

  quint8 mkey[4] = {0, 0, 0, 0};
  if (masked) {
    if (buf_.size() < pos + 4) return false;
    for (int i = 0; i < 4; ++i) mkey[i] = static_cast<quint8>(buf_[pos + i]);
    pos += 4;
  }

  if (static_cast<quint64>(buf_.size() - pos) < len) return false;  // incomplete

  QByteArray payload = buf_.mid(pos, static_cast<int>(len));
  if (masked) {
    for (int i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<char>(static_cast<quint8>(payload[i]) ^ mkey[i & 3]);
    }
  }
  buf_.remove(0, pos + static_cast<int>(len));
  dispatch(fin, opcode, payload);
  return true;
}

void Parser::dispatch(bool fin, quint8 opcode, const QByteArray& payload) {
  // RFC 6455 §5.5 : les trames de contrôle sont bornées à 125 octets et ne se
  // fragmentent pas. Ni l'une ni l'autre de ces règles n'était appliquée — un
  // `ping` géant était en plus renvoyé intégralement en `pong`, doublant
  // l'allocation.
  const bool isControl = (opcode == OpClose || opcode == OpPing || opcode == OpPong);
  if (isControl && (payload.size() > kMaxControlPayload || !fin)) {
    errored_ = true;
    buf_.clear();
    if (onError) onError(QStringLiteral("invalid control frame"));
    return;
  }

  switch (opcode) {
    case OpClose: {
      quint16 code = 1005;  // "no status" per RFC 6455 §7.1.5
      QString reason;
      if (payload.size() >= 2) {
        code = static_cast<quint16>((static_cast<quint8>(payload[0]) << 8) |
                                    static_cast<quint8>(payload[1]));
        reason = QString::fromUtf8(payload.mid(2));
      }
      if (onClose) onClose(code, reason);
      return;
    }
    case OpPing:
      if (onPing) onPing(payload);
      return;
    case OpPong:
      if (onPong) onPong(payload);
      return;
    case OpText:
    case OpBinary:
      if (!fin) {
        // Une trame data non-FIN reçue alors qu'un message est déjà en cours
        // est une violation de protocole (RFC 6455 §5.4). Elle ÉCRASAIT
        // silencieusement le tampon de réassemblage.
        if (fragmentOpcode_ >= 0) {
          errored_ = true;
          buf_.clear();
          if (onError) onError(QStringLiteral("interleaved fragmented message"));
          return;
        }
        if (static_cast<quint64>(payload.size()) > kMaxInboundMessage) {
          errored_ = true;
          buf_.clear();
          if (onError) onError(QStringLiteral("fragmented message exceeds size cap"));
          return;
        }
        // Start of a fragmented message.
        fragmentOpcode_ = opcode;
        fragmentPayload_ = payload;
        return;
      }
      if (opcode == OpText && onText) onText(QString::fromUtf8(payload));
      // Binary single-frame messages are not used by the plugin; ignored.
      return;
    case OpContinuation: {
      if (fragmentOpcode_ < 0) {
        errored_ = true;
        buf_.clear();
        if (onError) onError(QStringLiteral("continuation frame with no message in progress"));
        return;
      }
      // Borne CUMULÉE : sans elle, une suite illimitée de trames de
      // continuation (chacune passant le plafond par trame) faisait croître ce
      // tampon jusqu'à l'OOM du processus OBS, en plein direct.
      if (static_cast<quint64>(fragmentPayload_.size()) +
              static_cast<quint64>(payload.size()) > kMaxInboundMessage) {
        errored_ = true;
        buf_.clear();
        fragmentOpcode_ = -1;
        fragmentPayload_.clear();
        if (onError) onError(QStringLiteral("fragmented message exceeds size cap"));
        return;
      }
      fragmentPayload_.append(payload);
      if (fin) {
        const int op = fragmentOpcode_;
        const QByteArray full = fragmentPayload_;
        fragmentOpcode_ = -1;
        fragmentPayload_.clear();
        if (op == OpText && onText) onText(QString::fromUtf8(full));
      }
      return;
    }
    default:
      errored_ = true;
      buf_.clear();
      if (onError) onError(QStringLiteral("unknown opcode"));
      return;
  }
}

}  // namespace semirprod::ws
