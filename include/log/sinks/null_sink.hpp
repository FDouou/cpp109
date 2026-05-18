#pragma once

#include "../sink.hpp"

namespace cpp109 {

class NullSink : public Sink {
protected:
    void write(const std::string&, const LogEvent&) override {}
};

} // namespace cpp109
