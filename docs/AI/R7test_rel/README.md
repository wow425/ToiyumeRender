# R7test_rel — Release 构建的 R7 验收证据（去重后）

## 为什么只有一张图

Release 配置下完整跑了一遍 `-R7Test`（11 步，10 张 PNG）。
其中 **9 张渲染图的 md5 与 Debug 版（`docs/AI/R7test2/`）逐字节完全相同**，因此按 md5 去重删掉，
只保留唯一不同的那一张。

## 逐张对照（Debug `docs/AI/R7test2/` vs Release 本目录）

| 文件 | Debug md5 | Release md5 | 结论 |
|---|---|---|---|
| `r7_00_textures_ggx.png` | `2a52634f375a0d8bddc338e79c56e289` | 同 | 去重删除 |
| `r7_01_A5a_both_off.png` | `a43b605977a626e747c91559dcb1bb7f` | 同 | 去重删除 |
| `r7_02_A5b_ggx_only.png` | `fdd4d354135f4ba57316a15f93012c93` | 同 | 去重删除 |
| `r7_03_A5c_textures_only.png` | `3c952bcbe770b9b66e59b8b4889661da` | 同 | 去重删除 |
| `r7_04_debug_basecolor.png` | `5ce00c38dc9710a97b2be9e4ce5f19b2` | 同 | 去重删除 |
| `r7_05_deferred_basecolor.png` | `033cd410e3de76c4d17a9c24d1bd4bde` | 同 | 去重删除 |
| `r7_06_dxr_basecolor_after_switch.png` | `5ce00c38dc9710a97b2be9e4ce5f19b2` | 同 | 去重删除 |
| `r7_07_with_reflection.png` | `ebaf182ffe07cd983893e4d3eafa9196` | 同 | 去重删除 |
| `r7_08_restored.png` | `2a52634f375a0d8bddc338e79c56e289` | 同 | 去重删除 |
| `r7_09_imgui_panel.png` | `4bbf6e27ad5e56b7ea45c642bc7a42a6` | `82caf07685e57c712d737f7452229825` | **保留** |

`r7_09_imgui_panel.png` 每次都不同，因为面板里有 FPS / 帧时间 / GPU frame 的实时文本
（Debug 与 Release 的帧时间不同，同一配置两次运行也不同）。这是预期之内，不是缺陷。

## 结论

**Release 与 Debug 的渲染输出逐字节一致**（9/9 张渲染图 md5 相同），
说明 R7 的贴图 + GGX 链路在两个配置下走的是同一条代码路径，没有优化等级导致的数值差异。
