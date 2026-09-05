// net.cppm — llmswitch.net：按供应商 baseUrl+key 拉取模型列表（接口模块）。
//
// 同步阻塞实现（每次调用独立 curl easy handle，线程安全），调用方负责线程：
// UI 会把调用派到任务线程（阶段B），UI 线程禁止直接调。curl 头只进实现单元。
export module llmswitch.net;

import std;

namespace net {

// 拉取模型列表。成功返回模型 id 列表（去重保序）；失败抛 std::runtime_error
// （中文消息）。apiFormat 取值同 Provider.apiFormat："" / "openai"（默认
// OpenAI 兼容）/ "anthropic" / "openai-responses"（模型列表端点与 openai
// 相同）。
export std::vector<std::string> fetchModels(std::string_view baseUrl,
                                            std::string_view apiKey,
                                            std::string_view apiFormat);

// 响应体解析（纯函数，便于测试）：兼容 {"data":[{"id":...}]} 与
// {"models":[{"id":...}]} 两种形状（数组元素也可以是纯字符串）；坏 JSON 或
// 缺少列表键抛 std::runtime_error。id 去重保序。
export std::vector<std::string> parseModelIds(std::string_view body);

} // namespace net
