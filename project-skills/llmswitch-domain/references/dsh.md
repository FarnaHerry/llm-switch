# dsh 增量路由、凭据与模型契约

实现入口在 src/store_live.cpp，界面在 src/ui/providers_dsh.cpp。

## 单条操作与左右两列

dsh = `~/.dsh/settings.yaml` 行级改写，**单条增量的多路由**：
llm-pi-ai.providers 是「多供应商并存 + agent-default-model 指默认」的累加
map，写侧每个入口**只动自己那一条**——`writeDshEntry`（私有的单条原位 upsert
与指针改指，目标键由 `dshEntryKeyFor` 解析）与 `eraseDshEntry`（私有的单条
删除）是全部实现，
公开入口 `writeDshProvider`（每行「写入 / 更新」）/ `adoptDshProvider`
（收编手写裸键：**只记进本地列表，live 一字不动**）/
`removeDshProvider`（只删 live 那一条，含未纳管的 dsh 条目）/ 增改复制删
与 `switchTo`（切换 = 只改 agent-default-model；目标条目不在 live 里时补写
一条，避免指针悬空）都走它们；右列删除走 `removeProvider` 的 `eraseLive`
参数——条目同时在 live 里时页面先用三键确认框问「只删本应用」（false = 只
收回本地留存，settings.yaml 一字不动，那条随即成为左列的未纳管手写路由）
还是「连同 dsh 一起删」（true，默认 = 历史行为）；`dshLiveProviders` 是左列实况快照
（settings.yaml 里没有内置 deepseek-official 时补一条 `builtin` 合成行，
不能收编/删除；没有 agent-default-model 块时官方行就是「使用中」）。
**「当前用哪条」这个动作在左列**：`setDshDefaultKey` 只改
agent-default-model（未纳管的手写路由也能设为默认——dsh 的默认本来就是个
键名；合成官方行上是「清回内置官方路由」= 删掉那个块），右列只留写入 / 收编 /
删除。右列每行的「写入 / 更新」图标是 `write.svg`（文件 + 从边界插进去的一条
墨块 = 只写这一条），不用 Icon Set 的 `upload.svg`（底座 + 竖箭头，24px 下认不
出语义、和整组写入的批量动作混同）。

## 显式整组同步

整组重建只剩 `syncDshProviders` 一个入口——供应商页的「全部写入 dsh」按钮；
新条目写 `llmswitch-<id>`，已收编原生键原位更新
（api 字段复用 pi 三档映射，追加在 providers 块尾），清掉组外孤儿 `llmswitch-*`。
**别把增删改做成整组重建**：那等于把本地全部供应商一次性推给 dsh，用户在
左列删掉/整理过的条目下一次本地增删就会被补回来。别家手写条目 / 内置
deepseek-official / 无关键 / 注释一律不动；`agent-default-model` 只在需要
时改（显式切换或清除、原默认指向已删供应商时清回内置官方路由）。

## 原生键与凭据

**`llmswitch-` 前缀只属于本应用自己创建的条目**：收编不改名，原生条目保持
它自己的键与 `apiKeyEnv`（密钥照常更新在同一个引用名下），所以
`dshEntryKeyFor` 按「带前缀的优先、否则原生键即 id」解析目标键——写/切换/
删除都走它，谁写的两边一眼能分。密钥只写
`~/.dsh/.credentials.yaml`（**version 1 布局：顶层只允许 version / refs /
records，引用名写进 `refs:` 块内**（缩进 2）；写进顶层会被 dsh 判
`unknown top-level key` 而拒绝整份文档、所有密钥一起失效——旧版本写到顶层的
`LLMSWITCH_*` 残留会在下一次写入时迁回 refs 块；没有 version 的预发布 flat
布局仍写顶层，空文件直接建 version 1 骨架。refs: {} 可摊平成块风格，
其它 flow 值明确报错，不硬写。旧顶层同名引用迁移以 refs 内已有值为准。
apiKeyEnv 引用只增改，不代清无引用的旧键，目录 0700、文件 0600）。
读侧 adopt/importLive 的 version 1 凭据从 refs 块读取，预发布 flat 布局依实现兼容。

## 模型能力与热更新

首个模型条目按 Provider 的官方能力字段写：`contextWindow` / `maxTokens`（容量，
0 = 不声明）、`input: [text, image]`（请求模态，声明 image 才让手工路由
收图片附件；text 是底座，非空清单必然带 text）、`reasoningEfforts`
（键固定加引号，"off" 留空 = 不发思考参数，其余写成同名拼写）——三者与
dsh 官方设置页编辑的是同一组（PiAiModelProfile），不声明时 dsh 依次退回
已装 catalog 的值、再退回路由默认（上下文 262144 / 输出 32768 / 模态
[text]），所以 0 与空清单一律当「不声明」不猜；agent-default-model 块
重写时保留用户在里面选的 `reasoningEffort`（dsh 的模型选择状态，不属本
应用的模型，但每切一次都被清掉就是丢官方设置）；两份 YAML 都被 dsh
热监听 → 切换即时生效
（needsRestart=false）。

## 官方恢复与 YAML 风格

restoreOfficial 删 agent-default-model 块与
llmswitch-* 条目回到内置 deepseek-official 路由；**行级改写只懂块风格**，
所以读（detectCurrent/importLive）写（switchTo/restoreOfficial）之前都先
把 providers 的 flow 风格值摊平成块风格（flow 里带引号的 `"null"` 是字符串、
裸 null/~/Null 与空值同义，摊平后统一写空值），否则往 flow 块后插块条目会
写出非法 YAML 且切换静默失效。

能力写入顺序为 contextWindow、maxTokens、input、reasoningEfforts；档位顺序
为 off/minimal/low/medium/high/xhigh/max，读取接受块/同行 flow、容量 K/M。
每条路由单独统计首个模型档位，不能跨路由复用计数。非空模态自动补 text，
勾图像带上文本，取消文本清除声明。flow 摊平支持同行和换行、嵌套 map/seq、
引号内 #/花括号，键值与顺序保留；仅 flow 内部注释不保留，见已知取舍。
双列列表还须遵守 [UI 性能预算](../../llmswitch-ui/references/provider-performance.md)；
flow 注释限制见 [已知取舍](known-tradeoffs.md)。
