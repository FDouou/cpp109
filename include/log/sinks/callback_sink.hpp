#pragma once

#include "../sink.hpp"

#include <functional>

namespace cpp109 {

// ──────────────────────────────────────────────────────────
// 模块四·CallbackSink：自定义回调输出
//
// 功能：
//   - 用户提供回调函数，每条日志都会触发
//   - 适用场景：发送到网络、写入数据库、第三方监控集成
// ──────────────────────────────────────────────────────────

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
