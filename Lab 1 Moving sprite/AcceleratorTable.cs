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

    private IntPtr _handle;

    /// <summary>
    /// Создаёт и регистрирует таблицу из трёх комбинаций выхода.
    /// Все сочетания проходят через TranslateAccelerator и приводят к WM_COMMAND.
    /// </summary>
    internal AcceleratorTable()
    {
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
        return commandId is CmdExitCtrlQ or CmdExitAltX or CmdExitEscape;
    }

    /// <summary>Освобождает таблицу — WinAPI не делает это автоматически при завершении процесса.</summary>
    public void Dispose()
    {
        if (_handle != IntPtr.Zero)
        {
            Win32Interop.DestroyAcceleratorTable(_handle);
            _handle = IntPtr.Zero;
        }
    }
}
