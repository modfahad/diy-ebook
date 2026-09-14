#include "net/protocol.h"

#include <string.h>

#include "qpk/qpk_format.h"

namespace net {

const char* ErrorCodeName(Error error) {
  switch (error) {
    case Error::kOk:                 return "OK";
    case Error::kBadRequest:         return "BAD_REQUEST";
    case Error::kUnauthorized:       return "UNAUTHORIZED";
    case Error::kNotFound:           return "NOT_FOUND";
    case Error::kOffsetMismatch:     return "OFFSET_MISMATCH";
    case Error::kChunkTooLarge:      return "CHUNK_TOO_LARGE";
    case Error::kSizeMismatch:       return "SIZE_MISMATCH";
    case Error::kVerifyFailed:       return "VERIFY_FAILED";
    case Error::kPackageRejected:    return "PACKAGE_REJECTED";
    case Error::kNoSpace:            return "NO_SPACE";
    case Error::kNoSession:          return "NO_SESSION";
    case Error::kStorageError:       return "STORAGE_ERROR";
    case Error::kNotInTransferMode:  return "NOT_IN_TRANSFER_MODE";
    default:                         return "BAD_REQUEST";
  }
}

uint16_t ErrorStatus(Error error) {
  switch (error) {
    case Error::kOk:                return kStatusOk;
    case Error::kUnauthorized:      return kStatusUnauthorized;
    case Error::kNotFound:
    case Error::kNoSession:         return kStatusNotFound;
    case Error::kOffsetMismatch:    return kStatusConflict;
    case Error::kChunkTooLarge:     return kStatusPayloadTooLarge;
    case Error::kSizeMismatch:
    case Error::kVerifyFailed:
    case Error::kPackageRejected:   return kStatusUnprocessable;
    case Error::kNoSpace:           return kStatusInsufficientStorage;
    case Error::kNotInTransferMode: return kStatusConflict;
    case Error::kStorageError:      return kStatusInsufficientStorage;
    default:                        return kStatusBadRequest;
  }
}

uint16_t PackageTypeFromName(const char* name) {
  if (name == nullptr) return 0;
  if (strcmp(name, "QURAN") == 0) {
    return static_cast<uint16_t>(qpk::PackageType::kQuran);
  }
  if (strcmp(name, "BOOK") == 0) {
    return static_cast<uint16_t>(qpk::PackageType::kBook);
  }
  if (strcmp(name, "TRANSLATION") == 0) {
    return static_cast<uint16_t>(qpk::PackageType::kTranslation);
  }
  if (strcmp(name, "TAFSIR") == 0) {
    return static_cast<uint16_t>(qpk::PackageType::kTafsir);
  }
  return 0;
}

const char* PackageTypeName(uint16_t type) {
  switch (static_cast<qpk::PackageType>(type)) {
    case qpk::PackageType::kQuran:       return "QURAN";
    case qpk::PackageType::kBook:        return "BOOK";
    case qpk::PackageType::kTranslation: return "TRANSLATION";
    case qpk::PackageType::kTafsir:      return "TAFSIR";
    default:                             return "UNKNOWN";
  }
}

const char* LibraryDirFor(uint16_t type) {
  switch (static_cast<qpk::PackageType>(type)) {
    case qpk::PackageType::kQuran:       return "QURAN";
    case qpk::PackageType::kBook:        return "BOOKS";
    case qpk::PackageType::kTranslation: return "TRANSLATIONS";
    case qpk::PackageType::kTafsir:      return "TAFSIR";
    default:                             return nullptr;
  }
}

}  // namespace net
