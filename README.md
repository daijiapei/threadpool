# Window/Linux 线程池

标准C开发,接口简单, 性能稳定, 支持window和linux平台. 

支持功能: 

核心线程数 : 生命周期一直存在的线程

最大线程数:  每次队列任务数量达到最大值时, 就会创建一条新的临时线程, 当任务队列变为空时, 所有临时线程都会退出

定时器: 支持毫秒级定时任务,  返回的定时器句柄, 在任意线程内关闭它都是线程安全的. 要注意的是, 在linux下超时值小于一秒时, 会按一秒来设置定时



loop queue模型:

支持转换为one loop per thread模型,  loop 模型有专门的函数,  loop是不会创建线程的, 它将会在本线程内进行

loop, 并且 loop 支持 嵌套, 就是在loop产生的回调函数中, 继续调用 loop.



兼容C++函数回调:

回调参数一般是 obj 和 event 这两个参数, 熟悉C++ class 对象的同学应该知道, 

class 的函数调用为 type * __thiscall type  this, type agr .... )

其中 this 可作为 obj 参数, event 可作为 arg, windows 下声明调用约定为 __stdcall

所以需要熟悉C++的同学, 可以用 class的成员函数作为回调对象

