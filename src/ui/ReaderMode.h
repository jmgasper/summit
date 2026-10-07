#pragma once
#include <string>

namespace summit {
// Embedded, pinned scripts. Evaluation must use an isolated content world.
const std::string& ReaderProbeScript();
const std::string& ReaderExtractScript();
std::string NewReaderURL();
}
