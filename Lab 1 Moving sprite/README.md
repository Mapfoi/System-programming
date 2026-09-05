# Основные этапы работы программы


## 1. Точка входа

`Main` создаёт окно и сразу входит в цикл сообщений. Пока `Run()` не вернётся, процесс живёт. Файл: `Program.cs`.

```csharp
[STAThread]
public static void Main()
{
    AppDomain.CurrentDomain.UnhandledException += OnUnhandledException;

    try
    {
        using SpriteWindow window = new SpriteWindow();
        window.Run();
    }
    catch (Exception ex)
    {
        ShowFatalError(ex.ToString());
    }
}
```

## 2. Подготовка: акселераторы, спрайт, класс окна, HWND

В конструкторе регистрируются горячие клавиши, спрайт ставится в центр, затем вызываются `RegisterClassEx` и `CreateWindowEx`. Без зарегистрированного класса окно не создастся. Файл: `SpriteWindow.cs`.

```csharp
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
```

Таблица акселераторов — три команды выхода (Ctrl+Q, Alt+X, Escape). Файл: `AcceleratorTable.cs`.

```csharp
Win32Interop.ACCEL[] entries =
[
    new Win32Interop.ACCEL
    {
        fVirt = (byte)(Win32Interop.FVIRTKEY | Win32Interop.FCONTROL),
        key = Win32Interop.VK_Q,
        cmd = CmdExitCtrlQ
    },
    new Win32Interop.ACCEL
    {
        fVirt = (byte)(Win32Interop.FVIRTKEY | Win32Interop.FALT),
        key = Win32Interop.VK_X,
        cmd = CmdExitAltX
    },
    new Win32Interop.ACCEL
    {
        fVirt = Win32Interop.FVIRTKEY,
        key = Win32Interop.VK_ESCAPE,
        cmd = CmdExitEscape
    }
];

_handle = Win32Interop.CreateAcceleratorTable(entries, entries.Length);
```

Класс окна привязывает `WndProc`. Окно — контейнер для ввода и отрисовки.

```csharp
private void RegisterWindowClass()
{
    Win32Interop.WNDCLASSEX wndClass = new Win32Interop.WNDCLASSEX
    {
        cbSize = (uint)Marshal.SizeOf<Win32Interop.WNDCLASSEX>(),
        style = 0,
        lpfnWndProc = StaticWndProc,
        hInstance = Win32Interop.GetModuleHandle(null),
        hCursor = Win32Interop.LoadCursor(IntPtr.Zero, Win32Interop.IDC_ARROW),
        hbrBackground = (IntPtr)(Win32Interop.COLOR_WINDOW + 1),
        lpszClassName = WindowClassName
    };

    if (Win32Interop.RegisterClassEx(ref wndClass) == 0)
    {
        uint error = Win32Interop.GetLastError();
        if (error != 1410)
        {
            throw new InvalidOperationException(
                $"RegisterClassEx завершился с ошибкой. GetLastError={error}");
        }
    }
}
```

```csharp
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
}
```

## 3. Цикл сообщений

Это ядро программы: пока `GetMessage` не получит `WM_QUIT`, приложение крутится здесь. Сначала `TranslateAccelerator` (Ctrl+Q / Alt+X / Esc → `WM_COMMAND`), иначе обычная диспетчеризация в `WndProc`.

```csharp
internal void Run()
{
    Win32Interop.ShowWindow(_windowHandle, Win32Interop.SW_SHOW);
    Win32Interop.UpdateWindow(_windowHandle);

    Win32Interop.MSG msg;

    while (Win32Interop.GetMessage(out msg, IntPtr.Zero, 0, 0) > 0)
    {
        if (Win32Interop.TranslateAccelerator(
                _windowHandle, _acceleratorTable.Handle, ref msg) != 0)
        {
            continue;
        }

        Win32Interop.TranslateMessage(ref msg);
        Win32Interop.DispatchMessage(ref msg);
    }
}
```

## 4. Диспетчер окна

Каждое сообщение — отдельный этап работы.

```csharp
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
                Win32Interop.PostQuitMessage(0);
                return IntPtr.Zero;

            default:
                return Win32Interop.DefWindowProc(hWnd, msg, wParam, lParam);
        }
    }
    catch (Exception ex)
    {
        Win32Interop.MessageBoxW(hWnd, ex.ToString(),
            "Ошибка в обработчике окна", Win32Interop.MB_ICONERROR);
        return IntPtr.Zero;
    }
}
```

## 5. Движение и перерисовка

WASD меняют координаты; `InvalidateRect` ставит в очередь `WM_PAINT`, а не рисует сразу.

```csharp
private IntPtr HandleKeyDown(IntPtr wParam)
{
    int virtualKey = wParam.ToInt32();

    if (_sprite.TryMove(virtualKey))
    {
        Win32Interop.InvalidateRect(_windowHandle, IntPtr.Zero, true);
    }

    return IntPtr.Zero;
}
```

Геометрия спрайта (`SpriteController.cs`):

```csharp
internal bool TryMove(int virtualKey)
{
    switch (virtualKey)
    {
        case Win32Interop.VK_W:
            return MoveUpIfWithinBounds();
        case Win32Interop.VK_S:
            return MoveDownIfWithinBounds();
        case Win32Interop.VK_A:
            return MoveLeftIfWithinBounds();
        case Win32Interop.VK_D:
            return MoveRightIfWithinBounds();
        default:
            return false;
    }
}
```

Отрисовка — GDI в `WM_PAINT`:

```csharp
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

    IntPtr brush = Win32Interop.CreateSolidBrush(0x000000FF);
    Win32Interop.FillRect(hdc, ref spriteRect, brush);
    Win32Interop.DeleteObject(brush);
    Win32Interop.EndPaint(hWnd, ref paintStruct);
    return IntPtr.Zero;
}
```

При изменении размера окна границы спрайта пересчитываются:

```csharp
private IntPtr HandleSize(IntPtr lParam)
{
    int width = lParam.ToInt32() & 0xFFFF;
    int height = (lParam.ToInt32() >> 16) & 0xFFFF;
    _sprite.UpdateClientSize(width, height);
    Win32Interop.InvalidateRect(_windowHandle, IntPtr.Zero, true);
    return IntPtr.Zero;
}
```

## 6. Выход

Акселератор → `WM_COMMAND` → `DestroyWindow` → `WM_DESTROY` → `PostQuitMessage` → `GetMessage` возвращает 0, `Run()` заканчивается.

```csharp
private IntPtr HandleCommand(IntPtr wParam)
{
    ushort commandId = (ushort)(wParam.ToInt32() & 0xFFFF);

    if (AcceleratorTable.IsExitCommand(commandId))
    {
        Win32Interop.DestroyWindow(_windowHandle);
    }

    return IntPtr.Zero;
}
```

## Цепочка целиком

`Main` → конструктор (акселераторы + `RegisterClassEx` + `CreateWindowEx`) → `ShowWindow` → цикл `GetMessage` → `WndProc` (клавиши / команда / размер / отрисовка / уничтожение). P/Invoke в `Win32Interop.cs` — только объявления API, логики этапов там нет.
