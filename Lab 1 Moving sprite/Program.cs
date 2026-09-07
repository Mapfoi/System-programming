namespace MovingSprite;

/// <summary>
/// Точка входа лабораторной работы: нативное Win32-окно со спрайтом и таблицей акселераторов.
/// </summary>
internal static class Program
{
    /// <summary>
    /// Запускает приложение: создаёт окно спрайта и входит в цикл сообщений Windows.
    /// STAThread нужен для корректной работы COM-совместимых компонентов Win32 на .NET.
    /// </summary>
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

    // Ловит исключения вне Main, которые иначе завершают процесс с кодом 0xE0434352
    private static void OnUnhandledException(object sender, UnhandledExceptionEventArgs e)
    {
        ShowFatalError(e.ExceptionObject?.ToString() ?? "Неизвестная ошибка");
    }

    private static void ShowFatalError(string message)
    {
        // hWnd = Zero — нет родительского окна
        // lpText — текст ошибки; lpCaption — заголовок
        // uType = MB_ICONERROR — иконка ошибки
        Win32Interop.MessageBoxW(
            IntPtr.Zero,
            message,
            "Ошибка запуска приложения",
            Win32Interop.MB_ICONERROR);
    }
}
