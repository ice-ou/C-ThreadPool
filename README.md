# C Thread Pool

学习Github上一个用C语言写的[线程池项目]([Pithikos/C-Thread-Pool: A minimal but powerful thread pool in ANSI C (github.com)](https://github.com/Pithikos/C-Thread-Pool))。创建该仓库用于自己学习。

线程池写得非常优雅，简单但功能强大

## 基本用法

1. 在源文件中包含头文件：`#include "thpool.h"`
2. 根据你想要的线程数量创建一个线程池：`threadpool thpool = thpool_init(4)`
3. 向池中添加工作：`thpool_add_work(thpool, (void*)function_p, (void*)arg_p)`

当工作队列中有工作，线程池中的线程会自动从队列中取出进行运行。如果想等待工作队列中的所有工作都完成后再继续，可以使用`thpool_wait(thpool)`。销毁线程池可以使用`thpool_destroy(thpool)`。

## API

| Function example                                             | Description                                                  |
| ------------------------------------------------------------ | ------------------------------------------------------------ |
| ***thpool_init(4)***                                         | 初始化一个拥有4个线程的线程池                                |
| ***thpool_add_work(thpool, (void&#42;)function_p, (void&#42;)arg_p)*** | 向线程池中加入新的工作（函数指针），同时传入函数参数         |
| ***thpool_wait(thpool)***                                    | 等待所有工作完成（包括正在运行的和工作队列中的）             |
| ***thpool_destroy(thpool)***                                 | 销毁线程池。如果当前有正在执行的工作，它将等待它们完成。     |
| ***thpool_pause(thpool)***                                   | 线程池中的所有线程将暂停，无论它们处于空闲状态还是在执行工作 |
| ***thpool_resume(thpool)***                                  | 从暂停恢复所用线程                                           |
| ***thpool_num_threads_working(thpool)***                     | 返回线程池中正在执行工作的线程                               |

## 总结文档

对于项目源码结构的分析在[文档]()

## 后续优化

To do......



