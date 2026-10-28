#include "ha_catalog.h"

#include <string.h>

// Sensors in a unit the panel converts, and on/off sensors for the off-grid role:
// every binary_sensor, plus unitless sensors with "grid" in their name, which is
// how the Sigen integration reports its grid connection status. The counter stops
// it at HA_CATALOG_MAX. Tabs and newlines in names and states are flattened so
// they cannot break a line.
const char HA_CATALOG_TEMPLATE[] =
    "{%- set ns = namespace(n=0) -%}"
    "{%- for s in states.sensor | list + states.binary_sensor | list -%}"
    "{%- set u = s.attributes.unit_of_measurement | default('', true) | string -%}"
    "{%- if ns.n < 300 and (u in ['W','kW','Wh','kWh','%','\xC2\xB0" "C']"
    " or (u == '' and (s.domain == 'binary_sensor' or 'grid' in s.object_id))) -%}"
    "{%- set ns.n = ns.n + 1 -%}"
    "{{ s.entity_id }}\t{{ u }}\t"
    "{{ s.name | replace('\\t', ' ') | replace('\\n', ' ') }}\t"
    "{{ s.state | replace('\\t', ' ') | replace('\\n', ' ') }}\n"
    "{% endif -%}"
    "{%- endfor -%}";

namespace {

// Copies [from, to) into a fixed field, cut to fit.
void copy_field(const char* from, const char* to, char* out, size_t size) {
  size_t length = static_cast<size_t>(to - from);
  if (length >= size) {
    length = size - 1;
  }
  memcpy(out, from, length);
  out[length] = '\0';
}

}  // namespace

size_t ha_catalog_parse(const char* text, size_t length, HaCandidate* out, size_t max) {
  if (text == nullptr || out == nullptr) {
    return 0;
  }
  size_t count = 0;
  const char* end = text + length;
  const char* line = text;
  while (line < end && count < max) {
    const char* line_end = static_cast<const char*>(memchr(line, '\n', end - line));
    if (line_end == nullptr) {
      line_end = end;
    }
    // Four fields, three tabs. Anything else is not a line this template wrote.
    const char* fields[4] = {line, nullptr, nullptr, nullptr};
    int found = 1;
    for (const char* p = line; p < line_end && found < 4; ++p) {
      if (*p == '\t') {
        fields[found++] = p + 1;
      }
    }
    if (found == 4) {
      HaCandidate& candidate = out[count];
      candidate = HaCandidate{};
      copy_field(fields[0], fields[1] - 1, candidate.entity_id, sizeof(candidate.entity_id));
      copy_field(fields[1], fields[2] - 1, candidate.unit, sizeof(candidate.unit));
      copy_field(fields[2], fields[3] - 1, candidate.name, sizeof(candidate.name));
      const char* state_end = line_end;
      if (state_end > fields[3] && state_end[-1] == '\r') {
        --state_end;
      }
      copy_field(fields[3], state_end, candidate.state, sizeof(candidate.state));
      if (ha_entity_id_valid(candidate.entity_id)) {
        ++count;
      }
    }
    line = line_end + 1;
  }
  return count;
}

bool ha_catalog_suits(HaValueKind kind, const HaCandidate& candidate) {
  const HaUnit unit = ha_unit_from_text(candidate.unit);
  switch (kind) {
    case HaValueKind::Power:
      return unit == HaUnit::Watts || unit == HaUnit::Kilowatts;
    case HaValueKind::Energy:
      return unit == HaUnit::WattHours || unit == HaUnit::KilowattHours;
    case HaValueKind::Percent:
      return unit == HaUnit::Percent;
    case HaValueKind::Temperature:
      return unit == HaUnit::Celsius;
    case HaValueKind::Boolean:
      return candidate.unit[0] == '\0';
  }
  return false;
}
