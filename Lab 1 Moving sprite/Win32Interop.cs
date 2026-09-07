using System.Runtime.InteropServices;

namespace MovingSprite;

/// <summary>
/// Централизует P/Invoke-вызовы Win32 API и связанные структуры данных.
/// Выделение в отдельный класс изолирует платформенные детали от логики приложения.
/// </summary>
internal static class Win32Interop
{
    /// <summary>Тип функции обратного вызова для оконной процедуры Windows.</summary>
    internal delegate IntPtr WndProcDelegate(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    /// <summary>
    /// Сообщение из очереди ввода Windows.
    /// Используется в цикле GetMessage и при вызове TranslateAccelerator.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct MSG
    {
        internal IntPtr hwnd;
        internal uint message;
        internal IntPtr wParam;
        internal IntPtr lParam;
        internal uint time;
        internal POINT pt;
    }

    /// <summary>
    /// Координаты точки внутри MSG.
    /// Вынесены в отдельную структуру для корректного выравнивания полей на x64.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct POINT
    {
        internal int x;
        internal int y;
    }

    /// <summary>
    /// Элемент таблицы акселераторов: связка модификаторов, клавиши и ID команды.
    /// Позволяет Windows обрабатывать горячие клавиши до развёртывания WM_KEYDOWN.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct ACCEL
    {
        internal byte fVirt;
        internal ushort key;
        internal ushort cmd;
    }

    /// <summary>Расширенный дескриптор класса окна для RegisterClassEx.</summary>
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct WNDCLASSEX
    {
        internal uint cbSize;
        internal uint style;
        internal WndProcDelegate lpfnWndProc;
        internal int cbClsExtra;
        internal int cbWndExtra;
        internal IntPtr hInstance;
        internal IntPtr hIcon;
        internal IntPtr hCursor;
        internal IntPtr hbrBackground;
        internal string lpszMenuName;
        internal string lpszClassName;
        internal IntPtr hIconSm;
    }

    /// <summary>Контекст рисования клиентской области при обработке WM_PAINT.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct PAINTSTRUCT
    {
        internal IntPtr hdc;
        internal int fErase;
        internal RECT rcPaint;
        internal int fRestore;
        internal int fIncUpdate;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)]
        internal byte[] rgbReserved;
    }

    /// <summary>Прямоугольник в координатах клиентской области окна.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct RECT
    {
        internal int left;
        internal int top;
        internal int right;
        internal int bottom;
    }

    internal const int WM_DESTROY = 0x0002;
    internal const int WM_SIZE = 0x0005;
    internal const int WM_PAINT = 0x000F;
    internal const int WM_KEYDOWN = 0x0100;
    internal const int WM_COMMAND = 0x0111;

    internal const int VK_W = 0x57;
    internal const int VK_A = 0x41;
    internal const int VK_S = 0x53;
    internal const int VK_D = 0x44;
    internal const int VK_ESCAPE = 0x1B;
    internal const int VK_Q = 0x51;
    internal const int VK_X = 0x58;
    internal const int VK_E = 0x45;

    internal const byte FVIRTKEY = 0x01;
    internal const byte FSHIFT = 0x04;
    internal const byte FCONTROL = 0x08;
    internal const byte FALT = 0x10;

    internal const int COLOR_WINDOW = 5;
    internal const uint WS_OVERLAPPEDWINDOW = 0x00CF0000;
    internal const int CW_USEDEFAULT = unchecked((int)0x80000000);
    internal const uint SW_SHOW = 5;
    internal const uint MB_ICONERROR = 0x00000010;
    internal const int IDC_ARROW = 32512;

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    internal static extern ushort RegisterClassEx(ref WNDCLASSEX lpwcx);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    internal static extern IntPtr CreateWindowEx(
        uint dwExStyle,
        string lpClassName,
        string lpWindowName,
        uint dwStyle,
        int x,
        int y,
        int nWidth,
        int nHeight,
        IntPtr hWndParent,
        IntPtr hMenu,
        IntPtr hInstance,
        IntPtr lpParam);

    [DllImport("user32.dll")]
    internal static extern bool ShowWindow(IntPtr hWnd, uint nCmdShow);

    [DllImport("user32.dll")]
    internal static extern bool UpdateWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    internal static extern IntPtr DefWindowProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    internal static extern int GetMessage(out MSG lpMsg, IntPtr hWnd, uint wMsgFilterMin, uint wMsgFilterMax);

    [DllImport("user32.dll")]
    internal static extern bool TranslateMessage(ref MSG lpMsg);

    [DllImport("user32.dll")]
    internal static extern IntPtr DispatchMessage(ref MSG lpmsg);

    [DllImport("user32.dll")]
    internal static extern void PostQuitMessage(int nExitCode);

    [DllImport("user32.dll")]
    internal static extern IntPtr LoadCursor(IntPtr hInstance, int lpCursorName);

    [DllImport("user32.dll")]
    internal static extern IntPtr BeginPaint(IntPtr hWnd, ref PAINTSTRUCT lpPaint);

    [DllImport("user32.dll")]
    internal static extern bool EndPaint(IntPtr hWnd, ref PAINTSTRUCT lpPaint);

    [DllImport("user32.dll")]
    internal static extern IntPtr CreateAcceleratorTable(ACCEL[] paccel, int cAccel);

    [DllImport("user32.dll")]
    internal static extern bool DestroyAcceleratorTable(IntPtr hAccel);

    [DllImport("user32.dll")]
    internal static extern int TranslateAccelerator(IntPtr hWnd, IntPtr hAccTable, ref MSG lpMsg);

    [DllImport("user32.dll")]
    internal static extern bool InvalidateRect(IntPtr hWnd, IntPtr lpRect, bool bErase);

    [DllImport("user32.dll")]
    internal static extern bool DestroyWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    internal static extern int FillRect(IntPtr hDC, ref RECT lprc, IntPtr hbr);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    internal static extern int MessageBoxW(IntPtr hWnd, string text, string caption, uint type);

    [DllImport("gdi32.dll")]
    internal static extern IntPtr CreateSolidBrush(uint color);

    [DllImport("gdi32.dll")]
    internal static extern bool DeleteObject(IntPtr hObject);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    internal static extern IntPtr GetModuleHandle(string? lpModuleName);

    [DllImport("kernel32.dll")]
    internal static extern uint GetLastError();
}
