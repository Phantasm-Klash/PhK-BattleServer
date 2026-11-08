# 原版 TNA 弹幕类型覆盖矩阵

参考 21330 条 / 复刻 23220 条逐弹状态；原版已解码 56 个 func，生产 Noop 0 个，依赖随机 5 个。

| 判定 | 数量 | 含义 |
|---|---:|---|
| MATCH | 56 | 逐 tick 位置/方向/大小/类型完全一致 |
| MISSING | 0 | 原版有实现、生产挂 FuncNoop |
| MISMATCH | 21 | 两边都有实现但数值不一致（**待修**） |
| RNG_DIFF | 2 | 差异来自 std::rand() vs DeterministicRng，需单独对齐 |
| UNKNOWN_REF | 7 | 原版本身未解码，不要求生产实现 |

## 需要处理的 func

| func | 判定 | 首个不一致 tick | 不一致字段 | 最大绝对差 |
|---:|---|---:|---|---:|
| 1 | MISMATCH | 0 | angle,life,speed,vx,x | 180.0 |
| 7 | RNG_DIFF | 1 | life | 1.0 |
| 13 | MISMATCH | 0 | angle,vx,vy,x,y | 270.0 |
| 18 | MISMATCH | 0 | angle,vx,vy,x,y | 307.978186 |
| 20 | MISMATCH | 0 | angle,life,vx,vy,x,y | 360.0 |
| 26 | MISMATCH | 0 | x,y | 587.0 |
| 30 | MISMATCH | 0 | hit_dis | 16.0 |
| 31 | MISMATCH | 0 | hit_dis | 39.0 |
| 36 | MISMATCH | 30 | opacity | 1.0 |
| 37 | MISMATCH | 0 | angle,opacity,vx,vy,x,y | 829.799973 |
| 38 | MISMATCH | 0 | angle,vx,vy,x,y | 275.939236 |
| 39 | MISMATCH | 0 | angle,speed,vx,vy,x,y | 534.0 |
| 48 | MISMATCH | 0 | angle,kind,speed,vx,vy,x,y | 330.0 |
| 50 | MISMATCH | 13 | angle,life,speed,vx,vy,x,y | 187.992842 |
| 58 | MISMATCH | 0 | angle,vx,vy,x,y | 451.0 |
| 62 | MISMATCH | 0 | angle,kind,life,speed,vx,vy,x,y | 2144638022.0 |
| 66 | MISMATCH | 0 | angle,vx,vy,x,y | 749.958878 |
| 67 | MISMATCH | 1 | angle,kind,life,speed,vx,x | 534.0 |
| 70 | RNG_DIFF | 1 | angle,kind,life,vx,vy,x,y | 6182354523.432096 |
| 74 | MISMATCH | 0 | opacity | 255.0 |
| 75 | MISMATCH | 0 | angle,vx,vy,x,y | 749.942861 |
| 82 | MISMATCH | 0 | angle,vx,vy,x,y | 180.0 |
| 83 | MISMATCH | 0 | angle,vx,vy,x,y | 460.0 |

## 完全一致

0, 2, 3, 4, 5, 6, 8, 11, 12, 14, 15, 16, 17, 19, 21, 22, 23, 24, 25, 27, 28, 29, 32, 33, 34, 35, 40, 41, 42, 43, 44, 45, 46, 47, 51, 52, 53, 54, 55, 57, 60, 61, 63, 64, 65, 71, 72, 73, 76, 77, 78, 79, 80, 81, 84, 85
