#pragma once

#include <Unknwn.h>
#include <new>

namespace dirsize {

// Generic class factory that creates instances of a COM class T.
// T must implement IUnknown and have a default constructor.
template <typename T>
class ClassFactory : public IClassFactory {
public:
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppvObject = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_refCount);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) delete this;
        return count;
    }

    // IClassFactory
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid,
                                             void** ppvObject) override {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        if (!ppvObject) return E_POINTER;

        T* obj = new (std::nothrow) T();
        if (!obj) return E_OUTOFMEMORY;

        HRESULT hr = obj->QueryInterface(riid, ppvObject);
        obj->Release(); // Release our initial ref; QI added one if it succeeded
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) {
            InterlockedIncrement(&m_lockCount);
        } else {
            InterlockedDecrement(&m_lockCount);
        }
        return S_OK;
    }

    static LONG GetLockCount() { return m_lockCount; }

private:
    LONG m_refCount = 1;
    static inline LONG m_lockCount = 0;
};

} // namespace dirsize
