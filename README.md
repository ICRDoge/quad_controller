# 4-20
stuck in the swing trajectory

# 4-21
start to store this project in github.(test)

# 4-22
把positionerrorgain调小,上楼梯就可以了

footplacement constraint的权重还需要调整
加入了friction cone的发现了法向量获取不知道效果如何
明天还要把elevation mapping那边再搞搞

# 4-23
footplacement constraint的权重调好了.现在是把b归一化了，不会出现某一个方向不稳定的现象了
elevation mapping调好了
friction cone暂时没问题

上楼梯的时候不稳定，需要解决

# 4-24
上楼梯不稳定的原因是之前做预测的时候Z方向没有进行赋值,导致有时候高度高了容易投影到错误的平面上去.
现在基本可以实现上下楼梯了

elevationmapping在跨沟的时候识别不到?
加入navigation planner?
之后上实物state estimator还是需要的
