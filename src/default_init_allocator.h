#pragma once

// A C++ allocator that default-initializes objects
// This principally useful for vectors of POD types, where default allocation is a no-op; so the vector can be resized "for free".
//
// Code shamelessly stolen from https://stackoverflow.com/a/21028912/8715474

#include <memory>

template <typename T, typename A = std::allocator<T>>
class default_init_allocator : public A {
    typedef std::allocator_traits<A> a_t;
public:
    // http://en.cppreference.com/w/cpp/language/using_declaration
    using A::A; // Inherit constructors from A

    template <typename U> struct rebind {
        using other =
            default_init_allocator
            <  U, typename a_t::template rebind_alloc<U>  >;
    };

    template <typename U>
    void construct(U* ptr)
        noexcept(std::is_nothrow_default_constructible<U>::value) {
        ::new(static_cast<void*>(ptr)) U;
    }

    template <typename U, typename...Args>
    void construct(U* ptr, Args&&... args) {
        a_t::construct(static_cast<A&>(*this),
            ptr, std::forward<Args>(args)...);
    }
};
