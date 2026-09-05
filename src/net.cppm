// net.cppm — llmswitch.net：按供应商 baseUrl+key 拉取模型列表（接口模块）。
//
// 同步阻塞实现（每次调用独立 curl easy handle，线程安全），调用方负责线程：
// UI 会把调用派到任务线程（阶段B），UI 线程禁止直接调。curl 头只进实现单元。
export module llmswitch.net;

import std;

namespace net {

// 拉取模型列表。成功返回模型 id 列表（去重保序）；失败抛 std::runtime_error
// （中文消息）。apiFormat 取值同 Provider.apiFormat 三档（见
// models::normalizeApiFormat）："anthropic" → GET {base}/v1/models；其余
// （openai-chat 默认 / openai-responses）→ GET {base}/models。
export std::vector<std::string> fetchModels(std::string_view baseUrl,
                                            std::string_view apiKey,
                                            std::string_view apiFormat);

// 响应体解析（纯函数，便于测试）：兼容 {"data":[{"id":...}]} 与
// {"models":[{"id":...}]} 两种形状（数组元素也可以是纯字符串）；坏 JSON 或
// 缺少列表键抛 std::runtime_error。id 去重保序。
export std::vector<std::string> parseModelIds(std::string_view body);

// 查询供应商用量：GET usageUrl + Bearer，用 jsonPath（点分取值路径，支持数组
// 下标，如 balance_infos.0.total_balance）从响应取值。成功返回文本（数字/
// 字符串/布尔都转成文本）；传输失败、非 2xx、路径取不到都抛
// std::runtime_error（中文消息）。同步阻塞，调用方负责线程（同 fetchModels）。
export std::string fetchUsage(std::string_view url, std::string_view apiKey,
                              std::string_view jsonPath);

// 点分路径取值（纯函数，便于测试）：段在对象上按键取、在数组上必须是十进制
// 下标；终值 string 原样、number 转十进制文本（整数不带小数点）、bool 转
// "true"/"false"。坏 JSON / 路径不存在 / 终值非标量抛 std::runtime_error。
export std::string extractByPath(std::string_view body, std::string_view dottedPath);

} // namespace net
