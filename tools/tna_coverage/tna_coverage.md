# 原版 TNA 弹幕类型覆盖矩阵

参考 7110 条 / 复刻 7740 条逐弹状态；原版已解码 56 个 func，生产 Noop 0 个，依赖随机 5 个。

| 判定 | 数量 | 含义 |
|---|---:|---|
| MATCH | 50 | 逐 tick 位置/方向/大小/类型完全一致 |
| MISSING | 0 | 原版有实现、生产挂 FuncNoop |
| MISMATCH | 26 | 两边都有实现但数值不一致（**待修**） |
| RNG_DIFF | 3 | 差异来自 std::rand() vs DeterministicRng，需单独对齐 |
| UNKNOWN_REF | 7 | 原版本身未解码，不要求生产实现 |

## 需要处理的 func

| func | 判定 | 首个不一致 tick | 不一致字段 | 最大绝对差 |
|---:|---|---:|---|---:|
| 1 | MISMATCH | 0 | angle,life,speed,vx,x | 180.0 |
| 6 | MISMATCH | 28 | life,x | 189.0 |
| 7 | RNG_DIFF | 1 | life | 1.0 |
| 8 | MISMATCH | 3 | life,x | 252.0 |
| 13 | MISMATCH | 0 | angle,vx,vy,x,y | 270.0 |
| 17 | MISMATCH | 5 | angle,vx,vy,x,y | 368.889556 |
| 18 | MISMATCH | 4 | angle,vx,vy,x,y | 277.186039 |
| 20 | MISMATCH | 0 | angle,life,vx,vy,x,y | 12753.374023 |
| 30 | MISMATCH | 4 | hit_dis | 16.0 |
| 31 | MISMATCH | 0 | hit_dis | 39.0 |
| 35 | MISMATCH | 0 | speed,vx,x | 360.0 |
| 36 | MISMATCH | 64 | opacity | 1.0 |
| 37 | MISMATCH | 0 | angle,opacity,vx,vy,x,y | 340.712386 |
| 38 | MISMATCH | 0 | angle,vx,vy,x,y | 275.939236 |
| 39 | MISMATCH | 1 | angle,speed,vx,vy,x,y | 361.986878 |
| 48 | MISMATCH | 4 | angle,speed,vx,vy,x,y | 211.561383 |
| 54 | MISMATCH | 0 | speed,vx,x | 360.0 |
| 58 | MISMATCH | 0 | angle,vx,vy,x,y | 451.0 |
| 61 | MISMATCH | 0 | angle,speed,vx,x | 188.0 |
| 62 | MISMATCH | 0 | angle,life,speed,vx,vy,x,y | 2136632134.0 |
| 66 | MISMATCH | 0 | angle,vx,vy,x,y | 449.0 |
| 67 | MISMATCH | 1 | angle,life,speed,vx,x | 214.0 |
| 70 | RNG_DIFF | 1 | angle,life,vx,vy,x,y | 4374220708.567904 |
| 71 | RNG_DIFF | 0 | x,y | 583.0 |
| 74 | MISMATCH | 0 | opacity | 251.0 |
| 75 | MISMATCH | 0 | angle,vx,vy,x,y | 449.017605 |
| 81 | MISMATCH | 0 | hit_dis,speed,vx,x | 360.0 |
| 82 | MISMATCH | 0 | angle,vx,vy,x,y | 270.0 |
| 83 | MISMATCH | 0 | angle,vx,vy,x,y | 460.0 |

## 完全一致

0, 2, 3, 4, 5, 11, 12, 14, 15, 16, 19, 21, 22, 23, 24, 25, 26, 27, 28, 29, 32, 33, 34, 40, 41, 42, 43, 44, 45, 46, 47, 50, 51, 52, 53, 55, 57, 60, 63, 64, 65, 72, 73, 76, 77, 78, 79, 80, 84, 85
