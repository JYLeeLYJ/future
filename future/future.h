//
// Created by qicosmos on 2020/7/22.
//

#ifndef FUTURE_DEMO_FUTURE_H
#define FUTURE_DEMO_FUTURE_H

#include "helper.h"
#include "try.h"
#include "shared_state.h"
#include "promise.h"

namespace ray {

enum class Lauch { Async, Pool, Sync };

struct EmptyExecutor {
  template <typename F> void submit(F &&fn) {}
};

template <typename T> class Future {
public:
  Future() = default;
  Future(const Future &) = delete;
  void operator=(const Future &) = delete;

  Future(Future &&fut) = default;
  Future &operator=(Future &&fut) = default;

  explicit Future(std::shared_ptr<SharedState<T>> state)
      : shared_state_(std::move(state)) {}

  bool Valid() const { return shared_state_ != nullptr; }

  template <typename F>
  Future<typename function_traits<F>::return_type> Then(F &&fn) {
    return Then(Lauch::Async, std::forward<F>(fn));
  }

  template <typename F>
  Future<typename function_traits<F>::return_type> Then(Lauch policy, F &&fn) {
    return ThenImpl(policy, (EmptyExecutor *)nullptr, std::forward<F>(fn));
  }

  template <typename F, typename Executor>
  Future<typename function_traits<F>::return_type> Then(Executor &executor,
                                                        F &&fn) {
    return ThenImpl(Lauch::Pool, &executor, std::forward<F>(fn));
  }

  T Get() {
    {
      std::unique_lock<std::mutex> lock(shared_state_->then_mtx_);
      switch (shared_state_->state_) {
      case FutureStatus::None:
        break;
      case FutureStatus::Timeout:
        throw std::runtime_error("timeout");
      case FutureStatus::Done:
        shared_state_->state_ = FutureStatus::Retrived;
      default:
        throw std::runtime_error("already retrieved");
      }
    }
    shared_state_->Wait();
    std::unique_lock<std::mutex> lock(shared_state_->then_mtx_);
    return GetImpl<T>();
  }

  template <typename Rep, typename Period>
  FutureStatus
  Wait_for(const std::chrono::duration<Rep, Period> &timeout_duration) const {
    return shared_state_->wait_for(timeout_duration);
  }

  template <typename Clock, typename Duration>
  FutureStatus WaitUntil(
      const std::chrono::time_point<Clock, Duration> &timeout_time) const {
    return shared_state_->WaitUntil(timeout_time);
  }

  void Wait() { shared_state_->Wait(); }

private:
  template <typename F, typename Executor>
  Future<typename function_traits<F>::return_type>
  ThenImpl(Lauch policy, Executor *executor, F &&fn) {
    static_assert(function_traits<F>::arity <= 1,
                  "Then must take zero or one argument");
    using FirstArg = typename function_traits<F>::first_arg_t;
    using return_type = typename function_traits<F>::return_type;
    Promise<return_type> next_promise;
    auto next_future = next_promise.GetFuture();

    auto func = MakeMoveWrapper(std::move(fn));
    auto next_prom = MakeMoveWrapper(std::move(next_promise));

    std::unique_lock<std::mutex> lock(shared_state_->then_mtx_);
    if (shared_state_->state_ == FutureStatus::None) {
      shared_state_->then_ = [policy, executor, func, next_prom,
          this](typename TryWrapper<T>::type &&t) mutable {
        ExecuteTask<FirstArg>(policy, executor, func, next_prom, std::move(t));
      };
    } else if (shared_state_->state_ == FutureStatus::Done) {
      typename TryWrapper<T>::type t;
      try {
        t = std::move(shared_state_->value_);
      } catch (const std::exception &e) {
        t = (typename TryWrapper<T>::type)(std::current_exception());
      }
      lock.unlock();

      ExecuteTask<FirstArg>(policy, executor, func, next_prom, std::move(t));
    } else if (shared_state_->state_ == FutureStatus::Timeout) {
      throw std::runtime_error("timeout");
    }

    return next_future;
  }

  template<typename FirstArg, typename F, typename Executor>
  void ExecuteTask(Lauch policy, Executor* executor, MoveWrapper<F> func, MoveWrapper<Promise<T>> next_prom, typename TryWrapper<T>::type&& t){
    auto arg = MakeMoveWrapper(std::move(t));
    auto task = [func, arg, next_prom, this]() mutable {
      auto result = Invoke<FirstArg>(func.move(), arg.move());
      next_prom->SetValue(std::move(result));
    };
    if (policy == Lauch::Async) {
      Async(std::move(task));
    } else if (policy == Lauch::Pool) {
      executor->submit(std::move(task));
    } else {
      task();
    }
  }

  template <typename R>
  absl::enable_if_t<std::is_void<R>::value> GetImpl() {}

  template <typename R>
  absl::enable_if_t<!std::is_void<R>::value, T> GetImpl() {
    return shared_state_->value_.Value();
  }

  template <typename U, typename F, typename Arg>
  auto Invoke(F fn, Arg arg) ->
      typename std::enable_if<!IsTry<U>::value && std::is_same<void, U>::value,
                              try_type_t<decltype(fn())>>::type {
    using type = decltype(fn());
    return try_type_t<type>(fn());
  }

  template <typename U, typename F, typename Arg>
  auto Invoke(F fn, Arg arg) ->
      typename std::enable_if<!IsTry<U>::value && !std::is_same<void, U>::value,
                              try_type_t<decltype(fn(arg.Value()))>>::type {
    using type = decltype(fn(arg.Value()));
    return try_type_t<type>(fn(arg.Value()));
  }

  template <typename U, typename F, typename Arg>
  auto Invoke(F fn, Arg arg) -> typename std::enable_if<
      IsTry<U>::value && !std::is_same<void, typename IsTry<U>::Inner>::value,
      try_type_t<decltype(fn(std::move(arg)))>>::type {
    using type = decltype(fn(std::move(arg)));
    return try_type_t<type>(fn(std::move(arg)));
  }

  template <typename U, typename F, typename Arg>
  auto Invoke(F fn, Arg arg) -> typename std::enable_if<
      IsTry<U>::value && std::is_same<void, typename IsTry<U>::Inner>::value,
      try_type_t<decltype(fn(std::move(arg)))>>::type {
    using type = decltype(fn(std::move(arg)));
    return try_type_t<type>(fn(std::move(arg)));
  }

  template <typename U, typename F, typename Arg>
  auto Invoke(F fn, Arg arg) -> typename std::enable_if<
      IsFuture<U>::value &&
          !std::is_same<void, typename IsFuture<U>::Inner>::value,
      try_type_t<decltype(fn(std::move(arg)))>>::type {
    using type = decltype(fn(std::move(arg)));
    return try_type_t<type>(fn(std::move(arg)));
  }

  std::shared_ptr<SharedState<T>> shared_state_;
};

namespace future_internal {
template <typename R, typename P, typename F, typename Tuple>
inline absl::enable_if_t<std::is_void<R>::value> SetValue(P &promise, F &&fn,
                                                   Tuple &&tp) {
  absl::apply(fn, std::move(tp));
  promise.SetValue();
}

template <typename R, typename P, typename F, typename Tuple>
inline absl::enable_if_t<!std::is_void<R>::value> SetValue(P &promise, F &&fn,
                                                    Tuple &&tp) {
  promise.SetValue(absl::apply(fn, std::move(tp)));
}

template <typename Executor, typename F, typename... Args>
Future<typename function_traits<F>::return_type>
inline AsyncImpl(Lauch policy, Executor *ex, F &&fn, Args &&... args) {
  using R = typename function_traits<F>::return_type;
  Promise<R> promise;
  auto future = promise.GetFuture();
  auto tp = std::forward_as_tuple(std::forward<Args>(args)...);
  auto wrap_tp = MakeMoveWrapper(std::move(tp));

  auto task = [promise, fn, wrap_tp]() mutable {
    try {
      SetValue<R>(promise, std::move(fn), wrap_tp.move());
    } catch (...) {
      promise.SetValue(std::current_exception());
    }
  };

  assert(policy!=Lauch::Sync);
  if (ex) {
    assert(policy == Lauch::Pool);
    ex->submit(std::move(task));
  } else {
    std::thread thd(std::move(task));
    thd.detach();
  }

  return promise.GetFuture();
}
}

//free function of future
template <typename F, typename... Args>
inline Future<typename function_traits<F>::return_type> Async(F &&fn,
                                                       Args &&... args) {
  return future_internal::AsyncImpl(Lauch::Async, (EmptyExecutor *)nullptr, std::forward<F>(fn),
                                    std::forward<Args>(args)...);
}

template <typename Executor, typename F, typename... Args>
inline Future<typename function_traits<F>::return_type> Async(Executor *ex, F &&fn,
                                                       Args &&... args) {
  return future_internal::AsyncImpl(Lauch::Pool, ex, std::forward<F>(fn),
                                    std::forward<Args>(args)...);
}

template <typename T>
inline Future<T> MakeReadyFuture(T &&value) {
  Promise<absl::decay_t<T>> promise;
  promise.SetValue(std::forward<T>(value));
  return promise.GetFuture();
}

inline Future<void> MakeReadyFuture() {
  Promise<void> promise;
  promise.SetValue();
  return promise.GetFuture();
}

template <typename T>
inline Future<T> MakeExceptFuture(std::exception_ptr &&e) {
  Promise<T> promise;
  promise.SetException(std::move(e));

  return promise.GetFuture();
}

template <typename T, typename E>
inline Future<T> MakeExceptFuture(E &&e) {
  Promise<T> promise;
  promise.SetException(std::make_exception_ptr(std::forward<E>(e)));

  return promise.GetFuture();
}
}
#endif //FUTURE_DEMO_FUTURE_H
