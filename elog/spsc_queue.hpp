#pragma once

#include <atomic>
#include <cassert>
#include <utility>

namespace elog::details {
// SPSC 批量交接通道(单生产者/单消费者)。
//
// 生产者把节点挂在本地 pending 链表上,flush() 把整批接到已发布链尾部后
// 一次 atomic store 发布;消费者 drain() 用一次 atomic exchange 取走整批。
// 双方都只做 exchange/store:没有 CAS 循环,没有 ABA,也没有"消费者删除
// 节点时生产者仍持过期指针"的回收竞态——节点由其创建者发布,由 drain 方
// 负责逐个 delete。
template <typename T>
class SpscQueue {
  public:
    struct Node {
        Node(T &&v, Node *n) : value(std::move(v)), next(n) {
        }

        T value;
        Node *next;
    };

    // ---- 生产者(仅单线程) ----
    void push(T v) {
        auto *n = new Node(std::move(v), pending_);
        if (!pending_tail_) {
            pending_tail_ = n;
        }
        pending_ = n;
    }

    void flush() {
        if (!pending_) {
            return;
        }
        pending_tail_->next = head_.load(std::memory_order_acquire);
        head_.store(pending_, std::memory_order_release);
        pending_ = nullptr;
        pending_tail_ = nullptr;
    }

    // ---- 消费者(仅单线程) ----
    // 返回 LIFO 链(含全部已发布节点),处理完由调用方逐个 delete;
    // 需要 FIFO 顺序时先经 to_fifo() 反转。
    Node *drain() {
        return head_.exchange(nullptr, std::memory_order_acquire);
    }

  private:
    std::atomic<Node *> head_{nullptr};
    Node *pending_{nullptr}; // 生产者私有,无竞争
    Node *pending_tail_{nullptr};
};

// reverse
template <typename T>
typename SpscQueue<T>::Node *to_fifo(typename SpscQueue<T>::Node *list) {
    typename SpscQueue<T>::Node *fifo = nullptr;
    while (list) {
        auto *n = list;
        list = list->next;
        n->next = fifo;
        fifo = n;
    }
    return fifo;
}
} // namespace elog::details