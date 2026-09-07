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
        // hWnd — созданное окно
        // nCmdShow = SW_SHOW — сделать окно видимым
        Win32Interop.ShowWindow(_windowHandle, Win32Interop.SW_SHOW);

        // hWnd — перерисовать клиентскую область после показа
        Win32Interop.UpdateWindow(_windowHandle);

        Win32Interop.MSG msg;

        while (true)
        {
            // lpMsg — куда записать сообщение
            // hWnd = Zero — все окна потока
            // wMsgFilterMin/Max = 0 — без фильтра
            if (Win32Interop.GetMessage(out msg, IntPtr.Zero, 0, 0) <= 0)
            {
                break;
            }

            // hWnd — окно-получатель команды
            // hAccTable — таблица акселераторов
            // lpMsg — сообщение из очереди (до TranslateMessage)
            if (Win32Interop.TranslateAccelerator(
                    _windowHandle, _acceleratorTable.Handle, ref msg) != 0)
            {
                continue;
            }

            // lpMsg — виртуальные клавиши -> WM_CHAR
            Win32Interop.TranslateMessage(ref msg);

            // lpMsg — передать сообщение в WndProc
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

        // hWnd, msg, wParam, lParam — прочие сообщения без активного окна
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
            // hInstance = Zero — стандартный курсор; lpCursorName = IDC_ARROW
            hCursor = Win32Interop.LoadCursor(IntPtr.Zero, Win32Interop.IDC_ARROW),
            hbrBackground = (IntPtr)(Win32Interop.COLOR_WINDOW + 1),
            lpszMenuName = null!,
            lpszClassName = WindowClassName,
            hIconSm = IntPtr.Zero
        };

        // lpwcx — заполненная WNDCLASSEX
        // (имя класса, WndProc, кисть фона, курсор)
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
        // dwExStyle = 0 — без расширенных стилей
        // lpClassName — имя класса из RegisterClassEx
        // lpWindowName — заголовок окна
        // dwStyle = WS_OVERLAPPEDWINDOW — обычное окно
        // x, y = CW_USEDEFAULT — позицию выбирает система
        // nWidth, nHeight — начальный размер (800×600)
        // hWndParent, hMenu = Zero — нет родителя и меню
        // hInstance — модуль текущего процесса
        // lpParam = Zero — нет доп. данных создания
        _windowHandle = Win32Interop.CreateWindowEx(
            0,
            WindowClassName,
            "Системное программирование: управление спрайтом",
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
                    // nExitCode = 0 — код выхода для GetMessage
                    Win32Interop.PostQuitMessage(0);
                    return IntPtr.Zero;

                default:
                    // hWnd, msg, wParam, lParam — прочие сообщения
                    return Win32Interop.DefWindowProc(hWnd, msg, wParam, lParam);
            }
        }
        catch (Exception ex)
        {
            // hWnd — окно-владелец; lpText — текст; lpCaption — заголовок;
            // uType = MB_ICONERROR — иконка ошибки
            Win32Interop.MessageBoxW(hWnd, ex.ToString(), "Ошибка в обработчике окна", Win32Interop.MB_ICONERROR);
            return IntPtr.Zero;
        }
    }

    /// <summary>
    /// WASD обрабатываются здесь, а не в таблице акселераторов: движение — непрерывный ввод, выход — команды.
    /// </summary>
    private IntPtr HandleKeyDown(IntPtr wParam)
    {
        // wParam — виртуальный код клавиши (WASD)
        int virtualKey = wParam.ToInt32();

        if (_sprite.TryMove(virtualKey))
        {
            // hWnd — окно; lpRect = Zero — вся клиентская область
            // bErase = true — стереть фон перед WM_PAINT
            Win32Interop.InvalidateRect(_windowHandle, IntPtr.Zero, true);
        }

        return IntPtr.Zero;
    }

    /// <summary>WM_COMMAND от TranslateAccelerator несёт ID команды в младшем слове wParam.</summary>
    private IntPtr HandleCommand(IntPtr wParam)
    {
        // младшее слово wParam — ID из ACCEL.cmd
        ushort commandId = (ushort)(wParam.ToInt32() & 0xFFFF);

        if (AcceleratorTable.IsExitCommand(commandId))
        {
            // hWnd — уничтожить окно (-> WM_DESTROY)
            Win32Interop.DestroyWindow(_windowHandle);
        }

        return IntPtr.Zero;
    }

    /// <summary>Пересчитывает границы спрайта при изменении размеров окна.</summary>
    private IntPtr HandleSize(IntPtr lParam)
    {
        // младшее слово lParam — новая ширина
        // старшее слово lParam — новая высота
        int width = lParam.ToInt32() & 0xFFFF;
        int height = (lParam.ToInt32() >> 16) & 0xFFFF;
        _sprite.UpdateClientSize(width, height);

        // hWnd — окно; lpRect = Zero — вся область; bErase = true
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

        // hWnd — окно
        // lpPaint — область перерисовки (заполняет ОС)
        IntPtr hdc = Win32Interop.BeginPaint(hWnd, ref paintStruct);

        Win32Interop.RECT spriteRect = new Win32Interop.RECT
        {
            left = _sprite.X,
            top = _sprite.Y,
            right = _sprite.X + SpriteController.SpriteWidth,
            bottom = _sprite.Y + SpriteController.SpriteHeight
        };

        // color = 0x000000FF — COLORREF BGR (синий)
        IntPtr brush = Win32Interop.CreateSolidBrush(0x000000FF);

        // hDC — контекст из BeginPaint
        // lprc — прямоугольник спрайта; hbr — кисть
        Win32Interop.FillRect(hdc, ref spriteRect, brush);

        // hObject — GDI-кисть после FillRect
        Win32Interop.DeleteObject(brush);

        // hWnd, lpPaint — конец BeginPaint/EndPaint
        Win32Interop.EndPaint(hWnd, ref paintStruct);
        return IntPtr.Zero;
    }
}
