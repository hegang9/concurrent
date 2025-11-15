#pragma once
#include <atomic> // 引入原子操作支持
#include <iostream>
#include <memory> // 引入智能指针支持

// 无锁栈的实现使用链表，头指针为栈顶，pop使用延迟删除技术
// 延迟删除方式的缺点：在高负荷场景中，to_be_deleted候删链表可能无休止地增加，导致实际上的内存泄漏。因此，延迟删除技术适用于pop操作频率较低的场景
// 延迟删除方式的优点：避免了复杂的内存回收机制，如引用计数和垃圾

template <typename T>
class LockFreeStack
{
private:
    std::atomic<int> threads_in_pop;   // 记录正在执行pop操作的线程数量
    std::atomic<node *> to_be_deleted; // 延迟删除的节点链表头指针

    void try_reclaim(node *old_head)
    {
        if (threads_in_pop == 1)
        {
            delete old_head; // 安全删除旧的栈顶节点
            node *nodes_to_delete = to_be_deleted.exchange(nullptr);

            if (!--threads_in_pop) // 这里重复检测threads_in_pop的值，因为在exchange操作期间，可能有其他线程进入pop操作
            {
                delete_nodes(nodes_to_delete);
            }
            else if (nodes_to_delete) // 若在node *nodes_to_delete = to_be_deleted.exchange(nullptr);之后有其他线程加入pop操作，则需要将提取出来的待删除节点重新链回to_be_deleted去
            {
                chain_pending_nodes(nodes_to_delete);
            }
        }
        else
        {
            chain_pending_nodes(old_head);
            --threads_in_pop;
        }
    }

    void chain_pending_nodes(node *nodes)
    {
        node *last = nodes;
        while (node *const next = last->next)
        {
            last = next;
        }
        chain_pending_nodes(nodes, last);
    }

    void chain_pending_nodes(node *first, node *last)
    {
        last->next = to_be_deleted.load();
        while (!to_be_deleted.compare_exchange_weak(last->next, first))
            ;
    }

    void chain_pending_nodes(node *one)
    {
        chain_pending_nodes(one, one);
    }

    // 删除一系列节点的辅助函数
    static void delete_nodes(node *nodes)
    {
        while (nodes)
        {
            node *next = nodes->next;
            delete nodes;
            nodes = next;
        }
    }

private:
    struct node
    {
        std::shared_ptr<T> data;
        node *next;
        // 此处将const放置在data类型前，确保数据成员不可修改，即使T是指针类型，也不能修改指针所指向的地址
        node(T const &data_) : data(std::make_shared<T>(data_)), next(nullptr) {}
    };
    std::atomic<node *> head; // 栈顶指针，使用原子操作保证线程安全

public:
    LockFreeStack() : head(nullptr)
    {
        std::cout << head.is_lock_free() << std::endl;      // 输出head是否支持无锁操作
        std::cout << ATOMIC_POINTER_LOCK_FREE << std::endl; // 输出系统对指针类型的无锁支持情况
    }

    ~LockFreeStack()
    {
        while (pop())
            ;
    }

    void push(T const &data)
    {
        node *const new_node = new node(data); // 创建新节点,操作的是线程私有的局部变量，无数据竞争
        new_node->next = head.load();          // 将新节点的next指向当前栈顶,head.load()获取head原子变量，由于操作的是new_node，不会有数据竞争
        // 使用compare_exchange_weak进行原子操作，确保栈顶指针的更新是线程安全的，无数据竞争，因为如果在上下两条代码之间，其他线程改变了head指针，那么上一句代码中存储的旧指针的值与新head的值将不匹配，因此将会重新执行循环，重新循环后，new_node->next会被自动更新为最新的head值，从而保证了数据的一致性。
        while (!head.compare_exchange_weak(new_node->next, new_node))
            ;
    }

    std::shared_ptr<T> pop()
    {
        ++threads_in_pop; // 增加正在执行pop操作的线程数量
        node *old_head = head.load();
        // 使用compare_exchange_weak进行原子操作，确保栈顶指针的更新是线程安全的，无数据竞争
        while (old_head && !head.compare_exchange_weak(old_head, old_head->next))
            ;
        std::shared_ptr<T> res;
        if (old_head)
            res.swap(old_head->data); // 交换智能指针,这样待删除节点中data的引用计数会减少1，当引用计数为0时，data指向的对象就会被销毁，相当于提前销毁data，减轻之后集中销毁的压力
        try_reclaim(old_head);        // 尝试删除旧的栈顶节点
        return res;                   // 返回弹出的数据
    }
};