# ZCode 原生条目与增量同步

入口为 src/store_zcode.cpp、src/store_live.cpp 与 src/ui/providers_zcode.cpp。
live 是 `~/.zcode/v2/config.json` 的 provider map；模型归一和能力以当前代码为准。

## 单条操作与原生字段

- writeZcodeEntry 是单条 upsert，原生键经 mergeZcodeEntry 原位合并；
  eraseZcodeEntry 是单条删除。writeZcodeProvider/adoptZcodeProvider/
  removeZcodeProvider、增改复制删与 switchTo 都复用这些实现。
- 原生条目收编不改名、不改写，只开始记入本应用；写入/更新保留原生字段，
  包括 options 其它键、systemDisabledReason 等。密钥直接在条目内，没有独立凭据库。
- builtin:* 不收编、不删除，普通每行操作不代管其启停。
- zcodeLiveProviders 返回 key/providerId/builtin/enabled/模型清单，左列是真实状态；
  “在 ZCode 中启用”调用 enableZcodeKey，未纳管原生条目也能直接启用。
  右列只留写入、收编、删除，每行写入使用 write.svg。
- 切换只翻 enabled，不重写内容。ZCode 自身允许多条同时启用，本应用仅保证
  自己托管的 llmswitch:* 互斥，不把所有原生或 builtin 条目都停掉。
- removeProvider 的 eraseLive 区分“只删本应用”与“连同 ZCode 一起删”，
  live 存在时用三键确认，默认保持连同删除的历史行为。

## 显式同步与官方恢复

- 整组重建只有 syncZcodeProviders：新条目写 llmswitch:<id>，已有原生键原位
  更新，清理孤儿 llmswitch:*；原生条目与其它无关字段保留。
  不把日常增删改接成整组同步，以免用户整理过的 live 条目又被全部补回。
- restoreOfficial 是明确的官方恢复动作：关闭 llmswitch:* 并重新启用第一个
  builtin:*；无内置条目时由用户在 ZCode 自选。不要把此例外扩展到普通启用操作。

## 导入与模型清单

- load 首次导入是全量：每个有凭据的 live provider 收入本地，相同端点+密钥
  原位更新并保留 id，enabled 条目设为 current，无凭据 OAuth 条目跳过。
- Provider.models 是不定长清单，空表示仅 model 一个；导入全读、写入全保留，
  默认模型保证在列。表单获取模型后保存完整列表，未拉取则保留已有清单。
- 协议到 kind 的映射复用当前 writer；基础 apiFormat 归一见共享配置。
  双列布局还须遵守 [UI 性能预算](../../llmswitch-ui/references/provider-performance.md)。
