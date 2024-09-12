#pragma once

#include <android/log.h>

#include <concepts>

#include "lsplant.hpp"
#include "type_traits.hpp"

#if defined(__LP64__)
#define LP_SELECT(lp32, lp64) lp64
#else
#define LP_SELECT(lp32, lp64) lp32
#endif

#define CREATE_HOOK_STUB_ENTRY(SYM, RET, FUNC, PARAMS, DEF)                                        \
    inline static struct : public lsplant::Hooker<RET PARAMS, SYM>{                                \
                               inline static RET replace PARAMS DEF} FUNC

#define CREATE_MEM_HOOK_STUB_ENTRY(SYM, RET, FUNC, PARAMS, DEF)                                    \
    inline static struct : public lsplant::MemHooker<RET PARAMS, SYM>{                             \
                               inline static RET replace PARAMS DEF} FUNC

#define RETRIEVE_FUNC_SYMBOL(name, ...)                                                            \
    (name##Sym = reinterpret_cast<name##Type>(lsplant::Dlsym(handler, __VA_ARGS__)))

#define RETRIEVE_MEM_FUNC_SYMBOL(name, ...)                                                        \
    (name##Sym = reinterpret_cast<name##Type::FunType>(lsplant::Dlsym(handler, __VA_ARGS__)))

#define RETRIEVE_FIELD_SYMBOL(name, ...)                                                           \
    (name = reinterpret_cast<decltype(name)>(lsplant::Dlsym(handler, __VA_ARGS__)))

#define CREATE_FUNC_SYMBOL_ENTRY(ret, func, ...)                                                   \
    typedef ret (*func##Type)(__VA_ARGS__);                                                        \
    inline static ret (*func##Sym)(__VA_ARGS__);                                                   \
    inline static ret func(__VA_ARGS__)

#define CREATE_MEM_FUNC_SYMBOL_ENTRY(ret, func, thiz, ...)                                         \
    using func##Type = lsplant::MemberFunction<ret(__VA_ARGS__)>;                                  \
    inline static func##Type func##Sym;                                                            \
    inline static ret func(thiz, ##__VA_ARGS__)

namespace lsplant {

using HookHandler = InitInfo;

template <size_t N>
struct FixedString {
    consteval inline FixedString(const char (&str)[N]) { std::copy_n(str, N, data); }
    char data[N] = {};
};

inline void *Dlsym(const HookHandler &handle, const char *name, bool match_prefix = false) {
    if (auto match = handle.art_symbol_resolver(name); match) {
        return match;
    } else if (match_prefix && handle.art_symbol_prefix_resolver) {
        return handle.art_symbol_prefix_resolver(name);
    }
    return nullptr;
}

template <typename Class, typename Return, typename T, typename... Args>
    requires(std::is_same_v<T, void> || std::is_same_v<Class, T>)
inline static auto memfun_cast(Return (*func)(T *, Args...)) {
    union {
        Return (Class::*f)(Args...);

        struct {
            decltype(func) p;
            std::ptrdiff_t adj;
        } data;
    } u{.data = {func, 0}};
    static_assert(sizeof(u.f) == sizeof(u.data), "Try different T");
    return u.f;
}

template <std::same_as<void> T, typename Return, typename... Args>
inline auto memfun_cast(Return (*func)(T *, Args...)) {
    return memfun_cast<T>(func);
}

template <typename, typename = void>
class MemberFunction;

template <typename This, typename Return, typename... Args>
class MemberFunction<Return(Args...), This> {
    using SelfType = MemberFunction<Return(This *, Args...), This>;
    using ThisType = std::conditional_t<std::is_same_v<This, void>, SelfType, This>;
    using MemFunType = Return (ThisType::*)(Args...);

public:
    using FunType = Return (*)(This *, Args...);

private:
    MemFunType f_ = nullptr;

public:
    MemberFunction() = default;

    MemberFunction(FunType f) : f_(memfun_cast<ThisType>(f)) {}

    MemberFunction(MemFunType f) : f_(f) {}

    Return operator()(This *thiz, Args... args) {
        return (reinterpret_cast<ThisType *>(thiz)->*f_)(std::forward<Args>(args)...);
    }

    inline operator bool() { return f_ != nullptr; }
};

// deduction guide
template <typename This, typename Return, typename... Args>
MemberFunction(Return (*f)(This *, Args...)) -> MemberFunction<Return(Args...), This>;

template <typename This, typename Return, typename... Args>
MemberFunction(Return (This::*f)(Args...)) -> MemberFunction<Return(Args...), This>;

template <typename, FixedString>
struct Hooker;

template <typename Ret, FixedString Sym, typename... Args>
struct Hooker<Ret(Args...), Sym> {
    inline static Ret (*backup)(Args...) = nullptr;

    inline static constexpr std::string_view sym = Sym.data;
};

template <typename, FixedString>
struct MemHooker;
template <typename Ret, typename This, FixedString Sym, typename... Args>
struct MemHooker<Ret(This, Args...), Sym> {
    inline static MemberFunction<Ret(Args...)> backup;
    inline static constexpr std::string_view sym = Sym.data;
};

template <typename T>
concept HookerType = requires(T a) {
    a.backup;
    a.replace;
};

template <HookerType T>
inline static bool HookSymNoHandle(const HookHandler &handler, void *original, T &arg) {
    if (original) {
        if constexpr (is_instance_v<decltype(arg.backup), MemberFunction>) {
            void *backup = handler.inline_hooker(original, reinterpret_cast<void *>(arg.replace));
            arg.backup = reinterpret_cast<typename decltype(arg.backup)::FunType>(backup);
        } else {
            arg.backup = reinterpret_cast<decltype(arg.backup)>(
                handler.inline_hooker(original, reinterpret_cast<void *>(arg.replace)));
        }
        return true;
    } else {
        return false;
    }
}

template <HookerType T>
inline static bool HookSym(const HookHandler &handler, T &arg, bool resolve_symbol = false) {
    auto original = resolve_symbol ? handler.art_symbol_resolver(arg.sym) : &arg.sym;
    return HookSymNoHandle(handler, const_cast<void *>(original), arg);
}

template <HookerType T, HookerType... Args>
inline static bool HookSyms(const HookHandler &handle, T &first, Args &...rest) {
    if (!(HookSym(handle, first) || ... || HookSym(handle, rest))) {
        __android_log_print(ANDROID_LOG_ERROR,
#ifdef LOG_TAG
                            LOG_TAG,
#else
                            "HookHelper",
#endif
                            "Hook Fails: %*s", static_cast<int>(first.sym.size()),
                            first.sym.data());
        return false;
    }
    return true;
}

}  // namespace lsplant
