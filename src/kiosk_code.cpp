#include "kiosk_code.h"

#include <ctype.h>
#include <string.h>

namespace {

constexpr size_t CODE_CHARS = 20;
// secrets.token_urlsafe(32), which is what the server issued before v0.38.
constexpr size_t OLD_TOKEN_CHARS = 43;

// A code character as the server reads it, or '\0' if no code contains it.
// Crockford base32 leaves out i, l, o and u; the first three look like digits
// and are read as them.
char code_char(char c) {
  c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  if (c == 'o') {
    return '0';
  }
  if (c == 'i' || c == 'l') {
    return '1';
  }
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z' && c != 'u')) {
    return c;
  }
  return '\0';
}

bool is_base64url(char c) {
  return isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
}

bool copy_out(char* out, size_t size, const char* value, size_t length) {
  if (length >= size) {
    return false;
  }
  memcpy(out, value, length);
  out[length] = '\0';
  return true;
}

}  // namespace

bool kiosk_code_tidy(const char* typed, char* out, size_t size) {
  if (out == nullptr || size == 0) {
    return false;
  }
  out[0] = '\0';
  if (typed == nullptr) {
    return false;
  }

  // No form has whitespace in it. A paste brings it along, and a code read off a
  // screen may be typed in spaced groups.
  char compact[OLD_TOKEN_CHARS + 1];
  size_t length = 0;
  for (const char* c = typed; *c != '\0'; ++c) {
    if (isspace(static_cast<unsigned char>(*c))) {
      continue;
    }
    if (length == OLD_TOKEN_CHARS) {
      return false;  // longer than anything the server issues
    }
    compact[length++] = *c;
  }

  char code[CODE_CHARS];
  size_t count = 0;
  bool is_code = true;
  for (size_t i = 0; i < length && is_code; ++i) {
    if (compact[i] == '-') {
      continue;
    }
    const char c = code_char(compact[i]);
    is_code = c != '\0' && count < CODE_CHARS;
    if (is_code) {
      code[count++] = c;
    }
  }
  if (is_code && count == CODE_CHARS) {
    return copy_out(out, size, code, count);
  }

  // An older token goes back exactly as issued, or it will not match.
  if (length != OLD_TOKEN_CHARS) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (!is_base64url(compact[i])) {
      return false;
    }
  }
  return copy_out(out, size, compact, length);
}
