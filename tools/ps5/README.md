# PS5 构建 / 部署 / 日志工具

本目录下的两个 shell 脚本用于 **SwitchRecomp → 越狱 PS5** 移植的日常迭代闭环：
改完代码 → 编译 → 部署 → 点实机 → 取日志 → 分析修复。

| 脚本 | 作用 |
|------|------|
| `build_deploy.sh` | 在 253 编译机上同步代码、编译、部署 `eboot.bin` 到 PS5 |
| `fetch_log.sh`    | 从 PS5 拉取一次运行日志回 Mac 本地，便于分析崩溃 |

两个脚本都在 **Mac 终端**执行（SSH 到 253 已配置免密）。

---

## 环境前提

- Mac 可免密 SSH 到 253：`swizard@192.168.88.253`
- 253 已就绪：PS5 工具链（prospero-clang 等）+ 已生成的 `libgame_code.a`
- PS5（`192.168.88.33`）在部署 / 取日志时需**开机并接入同一局域网**
- Mac 本地有 `python3`（`fetch_log.sh` 直连分支用；若没有会自动改走 253 中转）

---

## build_deploy.sh — 同步 + 编译 + 部署

```bash
./build_deploy.sh              # 完整流程: git 对齐 → 编译 → 部署
./build_deploy.sh --deploy-only # 跳过编译, 仅部署已生成的 eboot (PS5 之前离线时补部署)
```

执行步骤：

1. **同步代码** — `git fetch origin ps5 && git reset --hard origin/ps5`
   （见下方「重要约定」，这里**不用** `git pull`）
2. **编译** — `ps5build.py --manifest .../ps5_build_manifest.json`
   （runtime-only：复用 `libgame_code.a`，重编 23 个 runtime 对象）
3. **探测 PS5 FTP** — 离线则提示「开机后执行 `--deploy-only`」并退出，不浪费编译
4. **部署** — `ps5_deploy_eboot.py --mode replace --local <eboot> --run-dir <时间戳>`

部署完成后，到 PS5 上点一下游戏，再用 `fetch_log.sh` 取日志。

---

## fetch_log.sh — 拉日志回 Mac 本地

```bash
./fetch_log.sh
```

行为：

1. **优先 Mac 直连 PS5 的 WFM（:8888）** 拉取，日志直接落 Mac 本地，最干净。
2. **若直连失败**（网络不可达 / PS5 离线），自动改走 **253 中转**：
   SSH 到 253 跑 `ps5_read_log.py`，再 `scp` 整个 run 目录回 Mac。
3. 日志落在 `<项目根>/runs/ps5log_<时间戳>/`：
   - `data__switchrecomp__swr.log` — 完整运行日志
   - `summary.txt` — 崩溃 census / verdict 摘要
   - `timeline.json` — 结构化时间线
4. 末尾自动打印关键 verdict 行（FATAL / svcBreak / invalid memory / sentinel / nvn 等）。

> 注意：取的是 PS5 上 `/data/switchrecomp/swr.log` 的**当前**内容。
> 若游戏没点 / 没重启，拉到的可能是上一次 run 的旧日志——点完再跑一次即可。

---

## 重要约定

- **git 同步用 `fetch + reset --hard`，不是 `git pull`**
  253 的 `ps5` 分支会与远端分叉，`git pull`（merge）会 non-fast-forward 失败。
  始终用 `git fetch origin ps5 && git reset --hard origin/ps5` 对齐 `origin/ps5`。

- **PS5 离线不卡死**
  `build_deploy.sh` 部署前先探 FTP，离线就报「请开机后 `--deploy-only`」并退出，
  已生成的 `eboot.bin` 不会丢失，开机后一条命令即可部署。

- **`--deploy-only` 开关**
  PS5 离线时跑完整流程会「编译完但部署失败」，开机后用此开关**直接部署**已生成的
  `eboot.bin`，无需重编译（编译约 1–2 分钟）。

- **runtime-only 构建的边界**
  默认构建复用 `libgame_code.a`（游戏代码归档），只重编 runtime 对象。
  `cpu_helpers.c` 里的 SIMD 修复（UDOT/SDOT）被 `#include` 进 game code，**不在**本次
  构建内——只有全量 `switchrecomp` 重建才会把它打进包。若需要该修复，须另行触发全量重建。

---

## 典型一天

```bash
# 1) 改完代码并 push origin/ps5 后, 在 Mac 上:
./tools/ps5/build_deploy.sh        # 同步 + 编译 + 部署 (PS5 需开机)

# 2) PS5 上点一下游戏

# 3) 回 Mac 取日志:
./tools/ps5/fetch_log.sh           # 日志落在 runs/ps5log_*/ 下

# 4) 分析 runs/ps5log_*/summary.txt, 决定下一轮修复
```
