using System.Runtime.InteropServices;

namespace MovingSprite;

/// <summary>
/// Отдельное Win32-окно со спрайтом: регистрация класса, WndProc, отрисовка и цикл сообщений.
/// Объединяет WinAPI-вызовы в одном месте, чтобы Program.cs оставался точкой входа.
/// </summary>
internal sealed class SpriteWindow : IDisposable
{
    private const string WindowClassName = "MovingSpriteWindowClass";
    private const int InitialWidth = 800;
    private const int InitialHeight = 600;

    // Статический делегат WndProc — надёжнее для отладчика VS, чем instance-callback
    private static readonly Win32Interop.WndProcDelegate StaticWndProc = DispatchWindowMessage;
    private static SpriteWindow? s_activeInstance;

    private readonly AcceleratorTable _acceleratorTable;
    private SpriteController _sprite;
    private IntPtr _windowHandle;

    /// <summary>
    /// Создаёт окно, таблицу акселераторов и размещает спрайт в центре клиентской области.
    /// </summary>
    internal SpriteWindow()
    {
        s_activeInstance = this;
        _acceleratorTable = new AcceleratorTable();
        _sprite = new SpriteController(
            InitialWidth / 2 - SpriteController.SpriteWidth / 2,
            InitialHeight / 2 - SpriteController.SpriteHeight / 2,
            InitialWidth,
            InitialHeight);

        RegisterWindowClass();
        CreateMainWindow();
    }

    /// <summary>
    /// Показывает окно и запускает цикл GetMessage с TranslateAccelerator.
    /// TranslateAccelerator вызывается до TranslateMessage, как требует документация WinAPI.
    /// </summary>
    internal void Run()
    {
        Win32Interop.ShowWindow(_windowHandle, Win32Interop.SW_SHOW);
        Win32Interop.UpdateWindow(_windowHandle);

        Win32Interop.MSG msg;

        while (Win32Interop.GetMessage(out msg, IntPtr.Zero, 0, 0) > 0)
        {
            // Акселераторы обрабатываются первыми: комбинации выхода не должны попадать в WM_KEYDOWN
            if (Win32Interop.TranslateAccelerator(_windowHandle, _acceleratorTable.Handle, ref msg) != 0)
            {
                continue;
            }

            Win32Interop.TranslateMessage(ref msg);
            Win32Interop.DispatchMessage(ref msg);
        }
    }

    /// <summary>Освобождает таблицу акселераторов и сбрасывает ссылку на активный экземпляр.</summary>
    public void Dispose()
    {
        _acceleratorTable.Dispose();

        if (s_activeInstance == this)
        {
            s_activeInstance = null;
        }

        _windowHandle = IntPtr.Zero;
    }

    /// <summary>
    /// Статическая точка входа WndProc — Windows вызывает её из нативного кода.
    /// </summary>
    private static IntPtr DispatchWindowMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam)
    {
        if (s_activeInstance != null)
        {
            return s_activeInstance.WindowProcedure(hWnd, msg, wParam, lParam);
        }

        return Win32Interop.DefWindowProc(hWnd, msg, wParam, lParam);
    }

    /// <summary>Регистрирует класс окна — без этого CreateWindowEx не создаст HWND.</summary>
    private void RegisterWindowClass()
    {
        Win32Interop.WNDCLASSEX wndClass = new Win32Interop.WNDCLASSEX
        {
            cbSize = (uint)Marshal.SizeOf<Win32Interop.WNDCLASSEX>(),
            style = 0,
            lpfnWndProc = StaticWndProc,
            cbClsExtra = 0,
            cbWndExtra = 0,
            hInstance = Win32Interop.GetModuleHandle(null),
            hIcon = IntPtr.Zero,
            hCursor = Win32Interop.LoadCursor(IntPtr.Zero, Win32Interop.IDC_ARROW),
            hbrBackground = (IntPtr)(Win32Interop.COLOR_WINDOW + 1),
            lpszMenuName = null!,
            lpszClassName = WindowClassName,
            hIconSm = IntPtr.Zero
        };

        if (Win32Interop.RegisterClassEx(ref wndClass) == 0)
        {
            uint error = Win32Interop.GetLastError();

            // ERROR_CLASS_ALREADY_EXISTS (1410) — класс уже зарегистрирован в процессе, это не ошибка
            if (error != 1410)
            {
                throw new InvalidOperationException($"RegisterClassEx завершился с ошибкой. GetLastError={error}");
            }
        }
    }

    /// <summary>Создаёт HWND отдельного окна — контейнер для спрайта и ввода WASD.</summary>
    private void CreateMainWindow()
    {
        _windowHandle = Win32Interop.CreateWindowEx(
            0,
            WindowClassName,
            "Sprite controller",
            Win32Interop.WS_OVERLAPPEDWINDOW,
            Win32Interop.CW_USEDEFAULT,
            Win32Interop.CW_USEDEFAULT,
            InitialWidth,
            InitialHeight,
            IntPtr.Zero,
            IntPtr.Zero,
            Win32Interop.GetModuleHandle(null),
            IntPtr.Zero);

        if (_windowHandle == IntPtr.Zero)
        {
            throw new InvalidOperationException(
                $"CreateWindowEx завершился с ошибкой. GetLastError={Win32Interop.GetLastError()}");
        }
    }

    /// <summary>Диспетчер сообщений окна: движение, отрисовка, выход и завершение.</summary>
    private IntPtr WindowProcedure(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam)
    {
        try
        {
            switch (msg)
            {
                case Win32Interop.WM_KEYDOWN:
                    return HandleKeyDown(wParam);

                case Win32Interop.WM_COMMAND:
                    return HandleCommand(wParam);

                case Win32Interop.WM_SIZE:
                    return HandleSize(lParam);

                case Win32Interop.WM_PAINT:
                    return HandlePaint(hWnd);

                case Win32Interop.WM_DESTROY:
                    // PostQuitMessage завершает цикл GetMessage в Run()
                    Win32Interop.PostQuitMessage(0);
                    return IntPtr.Zero;

                default:
                    return Win32Interop.DefWindowProc(hWnd, msg, wParam, lParam);
            }
        }
        catch (Exception ex)
        {
            // Исключение внутри WndProc без try/catch приводит к 0xE0434352 при вызове из нативного кода
            Win32Interop.MessageBoxW(hWnd, ex.ToString(), "Ошибка в обработчике окна", Win32Interop.MB_ICONERROR);
            return IntPtr.Zero;
        }
    }

    /// <summary>
    /// WASD обрабатываются здесь, а не в таблице акселераторов: движение — непрерывный ввод, выход — команды.
    /// </summary>
    private IntPtr HandleKeyDown(IntPtr wParam)
    {
        int virtualKey = wParam.ToInt32();

        if (_sprite.TryMove(virtualKey))
        {
            // InvalidateRect ставит WM_PAINT в очередь — рисование не блокирует обработку ввода
            Win32Interop.InvalidateRect(_windowHandle, IntPtr.Zero, true);
        }

        return IntPtr.Zero;
    }

    /// <summary>WM_COMMAND от TranslateAccelerator несёт ID команды в младшем слове wParam.</summary>
    private IntPtr HandleCommand(IntPtr wParam)
    {
        ushort commandId = (ushort)(wParam.ToInt32() & 0xFFFF);

        if (AcceleratorTable.IsExitCommand(commandId))
        {
            // DestroyWindow порождает WM_DESTROY, где вызывается PostQuitMessage
            Win32Interop.DestroyWindow(_windowHandle);
        }

        return IntPtr.Zero;
    }

    /// <summary>Пересчитывает границы спрайта при изменении размеров окна.</summary>
    private IntPtr HandleSize(IntPtr lParam)
    {
        int width = lParam.ToInt32() & 0xFFFF;
        int height = (lParam.ToInt32() >> 16) & 0xFFFF;
        _sprite.UpdateClientSize(width, height);
        Win32Interop.InvalidateRect(_windowHandle, IntPtr.Zero, true);
        return IntPtr.Zero;
    }

    /// <summary>Рисует спрайт синим прямоугольником через GDI в контексте WM_PAINT.</summary>
    private IntPtr HandlePaint(IntPtr hWnd)
    {
        Win32Interop.PAINTSTRUCT paintStruct = new Win32Interop.PAINTSTRUCT
        {
            rgbReserved = new byte[32]
        };

        IntPtr hdc = Win32Interop.BeginPaint(hWnd, ref paintStruct);

        Win32Interop.RECT spriteRect = new Win32Interop.RECT
        {
            left = _sprite.X,
            top = _sprite.Y,
            right = _sprite.X + SpriteController.SpriteWidth,
            bottom = _sprite.Y + SpriteController.SpriteHeight
        };

        // COLORREF 0x000000FF — синий в формате BGR, как ожидает CreateSolidBrush
        IntPtr brush = Win32Interop.CreateSolidBrush(0x000000FF);
        Win32Interop.FillRect(hdc, ref spriteRect, brush);
        Win32Interop.DeleteObject(brush);

        Win32Interop.EndPaint(hWnd, ref paintStruct);
        return IntPtr.Zero;
    }
}
