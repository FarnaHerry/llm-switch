# TODO

## 供应商增量模式（dsh / zcode）

工具的 live 配置本身是「多供应商并存 + 一个默认指向」，切换只该改指向，
增删改该是增量的；本应用另有一份自己的留存清单，两边会各自漂移，所以页面
按左右两列对照（左 = live 实况、右 = 本地留存），逐条可写入/收编。

- [x] dsh：`~/.dsh/settings.yaml` 的 llm-pi-ai.providers 增量 upsert
  （`llmswitch-<id>` 条目按组重建、孤儿清理、别家手写条目不动，
  agent-default-model 只改指向），供应商页左右两列 + 「全部写入 dsh」/
  「全部收编」+ 逐条写入/收编/删除（ToolSpec.additiveProviders）。
- [ ] zcode：同一类增量模式（`~/.zcode/v2/config.json` 的 provider map 也是
  多供应商并存、enabled 互斥停用其余）。待照搬两个差异对照的左右双列与
  全部/逐条写入收编，并把 `zcode` 的 `ToolSpec.additiveProviders` 打开
  （当前仍按「切换 = 只留一条」的写法）。

## 发布与分发

- [ ] 为 Linux RPM/DEB 发布包加入正式签名与验证链路：使用 CI 专用签名密钥，
  发布公钥与指纹，并补充用户导入、校验和仓库 `gpgcheck` 配置文档。
