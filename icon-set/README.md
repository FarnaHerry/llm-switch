# llm-switch Icon Set v1.0

依据参考图 **Icon Set v1.0** 逐枚还原的 24×24 功能图标集：35 枚语义图标，
统一「无色 alpha-mask + 水墨材质」语言，可直接配合主题 tint 使用。

```
icon-set/
├── *.svg              35 枚 24×24 alpha-mask 图标（唯一前景色 #FFFFFF）
├── preview-light.png  浅色（晴石）预览总览
├── preview-dark.png   深色（石墨）预览总览
└── README.md
```

## 规范

- `width="24" height="24"`，`viewBox="0 0 100 100"`，墨迹占 88/100（四周 6 单位留白），
  保证同排图标视觉大小一致；
- 全文件只出现 `#FFFFFF` 一种颜色，深浅模式由运行时 tint 决定（深色 → 冷白，
  浅色 → 深石板蓝），不维护 `_dark` / `_light` 重复轮廓；
- 主笔画圆头圆角、允许轻微不对称，保留参考图的水墨轮廓；
- 每枚语义只保留一个文件；选中态由承载底块（raised 圆角底 / accent chip）表达。

## 图标清单

### 核心导航 Main（11）

| 文件 | 中文 | English | 意象 |
|------|------|---------|------|
| `home.svg` | 主页（未选中） | Home (Idle) | 莲花线稿 |
| `home_active.svg` | 主页（选中） | Home (Active) | 实心莲花 |
| `agents.svg` | Agent 管理 | Agents | 青铜鼎与火 |
| `providers.svg` | 供应商 | Providers | 葫芦 |
| `models.svg` | 模型 | Models | 太极 |
| `router.svg` | 路由 | Router | 牌坊 |
| `skills.svg` | 技能 | Skills | 竹简 |
| `mcp.svg` | MCP | MCP | 卷轴 |
| `sessions.svg` | 会话 | Sessions | 摊开的书 |
| `usage.svg` | 用量 | Usage | 算盘 |
| `settings.svg` | 设置 | Settings | 八卦镜 |

### 常用操作 Actions（11）

| 文件 | 中文 | English | 意象 |
|------|------|---------|------|
| `delete.svg` | 删除 | Delete | 拂尘 |
| `download.svg` | 下载 | Download | 云雨 |
| `upload.svg` | 上传 | Upload | 香炉 |
| `search.svg` | 搜索 | Search | 放大镜 |
| `refresh.svg` | 刷新 | Refresh | 循环箭头 |
| `add.svg` | 新增 | Add | 加号（简单通用，见下方改版说明） |
| `edit.svg` | 编辑 | Edit | 毛笔 |
| `import.svg` | 导入 | Import | 开箱向下 |
| `export.svg` | 导出 | Export | 开箱向上 |
| `backup.svg` | 备份 | Backup | 宝箱 |
| `restore.svg` | 恢复 | Restore | 回卷还档 |

### 状态提示 Status（8）

| 文件 | 中文 | English |
|------|------|---------|
| `success.svg` | 成功 | Success |
| `error.svg` | 错误 | Error |
| `warning.svg` | 警告 | Warning |
| `info.svg` | 信息 | Info |
| `loading.svg` | 加载中 | Loading |
| `more.svg` | 更多 | More |
| `disabled.svg` | 禁用 | Disabled |
| `processing.svg` | 运行中 | Processing |

### 其他 Others（5）

| 文件 | 中文 | English | 意象 |
|------|------|---------|------|
| `user.svg` | 用户 | User | 斗笠人像 |
| `api_key.svg` | 密钥 | API Key | 吊牌 |
| `link.svg` | 链接 | Link | 锁链 |
| `options.svg` | 选项 | Options | 三六边形 |
| `logout.svg` | 退出 | Logout | 出门 |

## 变体说明

参考图的「变体示例 Variants」行（未选中 / 选中 / 默认 / 暗淡 / 禁用）不是独立语义，
因此不额外生成重复轮廓文件：

- **未选中 / 选中**：`home.svg` 与 `home_active.svg` 是唯一位图不同的两支
  （线稿 / 实心）；应用内选中态仍推荐用承载底块表达，实心莲花供需要整枚换形的场景；
- **默认 / 暗淡 / 禁用**：同一 SVG 加 tint 与透明度（预览中暗淡取 45%），
  禁用可叠加 `disabled.svg` 语义；
- 参考图中同一意象的「默认 / 深色」两档同理，不复制文件。

## 对参考图的改版

`add.svg` 是唯一偏离参考图造型的一枚：参考图的「新增」是卷宗 + 加号，笔画多、
在小尺寸下糊成一团（16px 已经认不出是新增）。按「简单通用」的要求换成单一
圆头加号——两条 2 单位圆头笔画构成的十字（`M12 6v12` + `M6 12h12`），
墨迹 14/24，是各平台 add 图标的通用比例，16px / 20px / 24px 全部清楚可辨。
其余 34 枚仍与参考图一致。

## 与 `resources/images/` 的对应关系

本目录是独立交付的图标集原稿，未接入构建（`resources/` 会被 `huxerui_add_app`
注册进资源包，本目录不在此列）。若要把某一枚并入运行时图标，按下面的对照复制到
`resources/images/` 即可：

| 本目录 | 仓库现有运行时文件 |
|--------|-------------------|
| `usage.svg` | `stats.svg` |
| `delete.svg` | `trash.svg` |
| `home_active.svg` | 仓库暂无（选中态目前由底块表达） |
| 其余 32 枚 | 同名文件 |

## 来源与许可

| 范围 | 来源 | 许可 |
|------|------|------|
| 全部 35 枚 | 由用户提供的 **llm-switch Icon Set v1.0** 参考图逐枚矢量化（水墨轮廓保留，输出统一为白色 alpha-mask） | 同本仓库 |
