#pragma once

#include <khook.hpp>

#include <atomic>
#include <utility>
#include <vector>

namespace cfl {

class KeptHook
{
public:
	virtual ~KeptHook() = default;
	virtual void Activate() = 0;
	virtual void Deactivate() = 0;
};

class HookKeeper
{
public:
	template <class Hook, class... Args>
	Hook *Make(Args &&...args)
	{
		Hook *hook = new Hook(std::forward<Args>(args)...);
		hooks_.push_back(hook);
		return hook;
	}

	void DeactivateAll();

private:
	std::vector<KeptHook *> hooks_;
};

template <class Owner, class RET, class... ARGS>
class FunctionHook final : public KHook::Function<RET, ARGS...>, public KeptHook
{
public:
	using Base = KHook::Function<RET, ARGS...>;
	using Handler = KHook::Return<RET> (Owner::*)(FunctionHook &, ARGS...);

	FunctionHook(Owner *owner, Handler pre, Handler post)
		: Base(pre ? &Pre : nullptr, post ? &Post : nullptr), owner_(owner), pre_(pre), post_(post)
	{
	}

	void Attach(void *address)
	{
		address_ = address;
		active_.store(true, std::memory_order_release);
		this->Configure(address);
	}

	void *Address() const { return address_; }
	void Activate() override { active_.store(true, std::memory_order_release); }
	void Deactivate() override { active_.store(false, std::memory_order_release); }

private:
	static KHook::Return<RET> Pre(ARGS... args) { return Dispatch(false, args...); }
	static KHook::Return<RET> Post(ARGS... args) { return Dispatch(true, args...); }

	static KHook::Return<RET> Dispatch(bool post, ARGS... args)
	{
		auto *self = static_cast<FunctionHook *>(KHook::GetContext<Base>());
		if (!self->active_.load(std::memory_order_acquire))
			return {KHook::Action::Ignore};
		return (self->owner_->*(post ? self->post_ : self->pre_))(*self, args...);
	}

	Owner *owner_;
	Handler pre_;
	Handler post_;
	void *address_ = nullptr;
	std::atomic<bool> active_{false};
};

template <class Owner, class CLASS, class RET, class... ARGS>
class VirtualHook final : public KHook::Virtual<CLASS, RET, ARGS...>, public KeptHook
{
public:
	using Base = KHook::Virtual<CLASS, RET, ARGS...>;
	using Handler = KHook::Return<RET> (Owner::*)(VirtualHook &, CLASS *, ARGS...);

	VirtualHook(RET (CLASS::*function)(ARGS...), Owner *owner, Handler pre, Handler post)
		: Base(function, pre ? &Pre : nullptr, post ? &Post : nullptr), owner_(owner), pre_(pre), post_(post)
	{
	}

	void AttachInstance(CLASS *object)
	{
		active_.store(true, std::memory_order_release);
		this->Add(object);
	}

	void AttachVTable(void *vtable)
	{
		fakeObject_ = vtable;
		active_.store(true, std::memory_order_release);
		this->AddGlobal(reinterpret_cast<CLASS *>(&fakeObject_));
	}

	void Activate() override { active_.store(true, std::memory_order_release); }
	void Deactivate() override { active_.store(false, std::memory_order_release); }

private:
	static KHook::Return<RET> Pre(CLASS *object, ARGS... args) { return Dispatch(false, object, args...); }
	static KHook::Return<RET> Post(CLASS *object, ARGS... args) { return Dispatch(true, object, args...); }

	static KHook::Return<RET> Dispatch(bool post, CLASS *object, ARGS... args)
	{
		auto *self = static_cast<VirtualHook *>(KHook::GetContext<Base>());
		if (!self->active_.load(std::memory_order_acquire))
			return {KHook::Action::Ignore};
		return (self->owner_->*(post ? self->post_ : self->pre_))(*self, object, args...);
	}

	Owner *owner_;
	Handler pre_;
	Handler post_;
	void *fakeObject_ = nullptr;
	std::atomic<bool> active_{false};
};

}
