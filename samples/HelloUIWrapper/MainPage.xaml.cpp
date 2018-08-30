//
// MainPage.xaml.cpp
// Implementation of the MainPage class.
//

#include "pch.h"
#include "MainPage.xaml.h"

using namespace HelloUIWrapper;

using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Data;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;

using namespace std;

// The Blank Page item template is documented at https://go.microsoft.com/fwlink/?LinkId=402352&clcid=0x409

template<typename T>
static T win(T result) {
    if (!result) {
        // Retrieve the system error message for the last-error code.
        LPVOID lpMsgBuf;
        DWORD dw = GetLastError();
        FormatMessage(
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL,
            dw,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPTSTR)&lpMsgBuf,
            0, NULL);

        // Display the error message.
        OutputDebugStringW((LPCTSTR)lpMsgBuf);

        LocalFree(lpMsgBuf);
    }
    return result;
}

MainPage::MainPage()
{
	InitializeComponent();

    // Call `HelloUILibrary.dll!start`.
    if (HMODULE lib = win(LoadPackagedLibrary(L"HelloUILibrary.dll", 0))) {
        if (FARPROC func = win(GetProcAddress(lib, "start"))) {
            ((void(*)(void))func)();
        }

        win(FreeLibrary(lib));
    }
}