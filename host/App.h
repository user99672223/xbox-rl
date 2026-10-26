// App.h - implementation of the XboxRL.App runtimeclass declared in App.idl.
#pragma once
#include "App.g.h"

namespace winrt::XboxRL::implementation
{
    struct App : AppT<App>
    {
        App() = default;
        Windows::ApplicationModel::Core::IFrameworkView CreateView();
    };
}

namespace winrt::XboxRL::factory_implementation
{
    struct App : AppT<App, implementation::App>
    {
    };
}
