# TODO

## 供应商增量模式（dsh / zcode）

工具的 live 配置本身是「多供应商并存 + 一个默认指向」，切换只该改指向，
增删改该是增量的；本应用另有一份自己的留存清单，两边会各自漂移，所以页面
按左右两列对照（左 = live 实况、右 = 本地留存），逐条可写入/收编。

- [x] dsh：`~/.dsh/settings.yaml` 的 llm-pi-ai.providers **单条增量**
  （`writeDshProvider` / `adoptDshProvider` / `removeDshProvider` / 增改复制删 /
  切换各只动自己那一条，`syncDshProviders` 整组重建只留给显式的「全部写入
  dsh」按钮），供应商页左右两列 + 「全部写入 dsh」/「全部收编」+ 逐条
  写入/收编/删除；左列在文件里没有内置 deepseek-official 时补一条只读合成行
  （ToolSpec.additiveProviders）。
- [x] zcode：`~/.zcode/v2/config.json` 的 provider map 同一套**单条增量**
  （`writeZcodeProvider` / `adoptZcodeProvider` / `removeZcodeProvider` /
  增改复制删 / 切换各只动自己那一条，`syncZcodeProviders` 整组重建只留给显式的
  「全部写入 ZCode」按钮），供应商页左右两列 + 全部写入 / 全部收编 + 逐条
  写入/收编/删除（删除确认问「只删本应用」还是「连同 ZCode 一起删」）；
  `ToolSpec.additiveProviders` 已打开。与 dsh 的两处差别：默认指向是条目自己的
  `enabled`（互斥只覆盖本应用托管的 llmswitch:*），密钥就在条目里（没有独立
  凭据文档）。**收编不改写 live**：ZCode 原生条目保留它自己的键与内容，写入/
  更新原位合并、保留它自己维护的字段（options 其它键、systemDisabledReason），
  `builtin:*` 不收编不删除。

## 发布与分发

- [ ] 为 Linux RPM/DEB 发布包加入正式签名与验证链路：使用 CI 专用签名密钥，
  发布公钥与指纹，并补充用户导入、校验和仓库 `gpgcheck` 配置文档。
