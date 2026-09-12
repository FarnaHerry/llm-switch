// net.cppm — llmswitch.net：模型列表地址推导与响应解析（接口模块）。
//
// 全部为纯函数（便于测试）。实际网络请求统一走 HuxerUI HttpClient 的平台
// 原生异步路径（供应商页见 provider_network.cpp；本地路由上游见
// router_transport.cpp），本模块不依赖任何 HTTP 栈。
export module llmswitch.net;

import std;

namespace net {

// 根据已计算好的上游基础 URL 和上游格式拼出模型列表地址。基础 URL 通常已经
// 包含 upstreamFormat 对应的 /anthropic 或 /v1 后缀；如果已经是 /models
// 端点，则原样返回，避免 /v1/v1/models 或 /models/models。
export std::string modelListUrl(std::string_view baseUrl,
                                std::string_view upstreamFormat);

// 返回模型列表探测候选地址，首项是按当前上游格式推导的地址，后续候选用于
// 兼容不同网关的 /models、/v1/models、/anthropic 前缀布局。结果去重保序。
export std::vector<std::string> modelListUrlCandidates(
    std::string_view baseUrl, std::string_view upstreamFormat);

// 响应体解析（纯函数，便于测试）：兼容 {"data":[{"id":...}]} 与
// {"models":[{"id":...}]} 两种形状（数组元素也可以是纯字符串）；坏 JSON 或
// 缺少列表键抛 std::runtime_error。id 去重保序。
export std::vector<std::string> parseModelIds(std::string_view body);

// 点分路径取值（纯函数，便于测试）：段在对象上按键取、在数组上必须是十进制
// 下标；终值 string 原样、number 转十进制文本（整数不带小数点）、bool 转
// "true"/"false"。坏 JSON / 路径不存在 / 终值非标量抛 std::runtime_error。
export std::string extractByPath(std::string_view body, std::string_view dottedPath);

} // namespace net
