#pragma once

#include "../sink.hpp"

#include <functional>

namespace cpp109 {

class CallbackSink : public Sink {
public:
    using Callback = std::function<void(const std::string&, const LogEvent& event)>;

    explicit CallbackSink(Callback cb): callback_(cb){}

    void set_callback(Callback cb){
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = cb;
    }

protected:
    void write(const std::string& formatted_msg, const LogEvent& event) override{
        callback_(formatted_msg,event);
    }

private:
    Callback callback_;
};

} // namespace cpp109
