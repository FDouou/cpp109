// ==========================================================================
// config_file.cpp — JSON 配置文件加载示例
//
// 演示对标 Django LOGGING 字典的使用方式：
//   1. 编写 log_config.json 配置文件
//   2. 在 main 开头加载
//   3. 之后 get_logger 自动匹配预设配置
//
// TODO: 实现后取消注释运行
// ==========================================================================

#include "log/log.hpp"

/*
int main() {
    // ── 加载 JSON 配置文件 ────────────────────────────
    cpp109::ConfigLoader::load_from_file("log_config.json");

    // ── 之后正常使用 ──────────────────────────────────
    auto admin = cpp109::get_logger("admin");
    admin->info("管理后台启动");

    auto goods = cpp109::get_logger("goods.import");  // 前缀回退命中 goods 配置
    goods->debug("商品导入开始, count={}", 500);

    auto unknown = cpp109::get_logger("something_else"); // 无匹配，走 root
    unknown->info("这条走 root");

    return 0;
}
*/

int main() {
    return 0;
}
