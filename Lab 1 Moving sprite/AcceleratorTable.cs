namespace MovingSprite;

/// <summary>
/// Обёртка над WinAPI CreateAcceleratorTable / TranslateAccelerator.
/// Реализует механизм «горячих клавиш» через системную таблицу акселераторов Windows.
/// </summary>
internal sealed class AcceleratorTable : IDisposable
{
    /// <summary>ID команды выхода: Ctrl+Q — стандартная «быстрая» комбинация в десктопных приложениях.</summary>
    internal const ushort CmdExitCtrlQ = 1001;

    /// <summary>ID команды выхода: Alt+X — второе независимое сочетание с другим модификатором.</summary>
    internal const ushort CmdExitAltX = 1002;

    /// <summary>ID команды выхода: Escape — аварийный выход без дополнительных модификаторов.</summary>
    internal const ushort CmdExitEscape = 1003;

    /// <summary>ID команды выхода: Ctrl+Alt+Q — тройное сочетание (два модификатора + клавиша).</summary>
    internal const ushort CmdExitCtrlAltQ = 1004;

    /// <summary>ID команды выхода: Ctrl+Shift+E — второе тройное сочетание без пересечения с WASD.</summary>
    internal const ushort CmdExitCtrlShiftE = 1005;

    private IntPtr _handle;

    /// <summary>
    /// Создаёт и регистрирует таблицу комбинаций выхода (включая два тройных сочетания).
    /// Все сочетания проходят через TranslateAccelerator и приводят к WM_COMMAND.
    /// </summary>
    internal AcceleratorTable()
    {
        Win32Interop.ACCEL[] entries =
        [
            // fVirt: FVIRTKEY|FCONTROL; key: Q; cmd: 1001
            // сочетание Ctrl+Q
            new Win32Interop.ACCEL
            {
                fVirt = (byte)(Win32Interop.FVIRTKEY | Win32Interop.FCONTROL),
                key = Win32Interop.VK_Q,
                cmd = CmdExitCtrlQ
            },
            // fVirt: FVIRTKEY|FALT; key: X; cmd: 1002
            // сочетание Alt+X
            new Win32Interop.ACCEL
            {
                fVirt = (byte)(Win32Interop.FVIRTKEY | Win32Interop.FALT),
                key = Win32Interop.VK_X,
                cmd = CmdExitAltX
            },
            // fVirt: FVIRTKEY; key: Escape; cmd: 1003
            // сочетание Escape
            new Win32Interop.ACCEL
            {
                fVirt = Win32Interop.FVIRTKEY,
                key = Win32Interop.VK_ESCAPE,
                cmd = CmdExitEscape
            },
            // fVirt: FVIRTKEY|FCONTROL|FALT; key: Q; cmd: 1004
            // сочетание Ctrl+Alt+Q
            new Win32Interop.ACCEL
            {
                fVirt = (byte)(Win32Interop.FVIRTKEY | Win32Interop.FCONTROL | Win32Interop.FALT),
                key = Win32Interop.VK_Q,
                cmd = CmdExitCtrlAltQ
            },
            // fVirt: FVIRTKEY|FCONTROL|FSHIFT; key: E; cmd: 1005
            // сочетание Ctrl+Shift+E
            new Win32Interop.ACCEL
            {
                fVirt = (byte)(Win32Interop.FVIRTKEY | Win32Interop.FCONTROL | Win32Interop.FSHIFT),
                key = Win32Interop.VK_E,
                cmd = CmdExitCtrlShiftE
            }
        ];

        // paccel — массив ACCEL
        // cAccel — число записей в таблице
        _handle = Win32Interop.CreateAcceleratorTable(entries, entries.Length);

        if (_handle == IntPtr.Zero)
        {
            throw new InvalidOperationException(
                $"CreateAcceleratorTable завершился с ошибкой. GetLastError={Win32Interop.GetLastError()}");
        }
    }

    /// <summary>Дескриптор таблицы для передачи в TranslateAccelerator.</summary>
    internal IntPtr Handle => _handle;

    /// <summary>
    /// Проверяет, соответствует ли ID команды одному из зарегистрированных выходов.
    /// </summary>
    /// <param name="commandId">Младшее слово wParam из WM_COMMAND.</param>
    /// <returns>True, если команда — запрос на завершение приложения.</returns>
    internal static bool IsExitCommand(ushort commandId)
    {
        return commandId is CmdExitCtrlQ or CmdExitAltX or CmdExitEscape
            or CmdExitCtrlAltQ or CmdExitCtrlShiftE;
    }

    /// <summary>Освобождает таблицу — WinAPI не делает это автоматически при завершении процесса.</summary>
    public void Dispose()
    {
        if (_handle != IntPtr.Zero)
        {
            // hAccel — дескриптор из CreateAcceleratorTable
            Win32Interop.DestroyAcceleratorTable(_handle);
            _handle = IntPtr.Zero;
        }
    }
}
