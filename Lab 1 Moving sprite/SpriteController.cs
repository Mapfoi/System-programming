namespace MovingSprite;

/// <summary>
/// Логическая модель спрайта: позиция и ограничения перемещения в клиентской области.
/// Отделена от WinAPI, чтобы обработчик WM_KEYDOWN не смешивал геометрию с системными сообщениями.
/// </summary>
internal sealed class SpriteController
{
    /// <summary>Ширина спрайта — используется при проверке правой границы окна.</summary>
    internal const int SpriteWidth = 50;

    /// <summary>Высота спрайта — используется при проверке нижней границы окна.</summary>
    internal const int SpriteHeight = 50;

    /// <summary>Шаг перемещения за одно нажатие WASD — фиксированный для предсказуемого управления.</summary>
    private const int MoveStep = 5;

    private int _x;
    private int _y;
    private int _clientWidth;
    private int _clientHeight;

    /// <summary>
    /// Размещает спрайт в заданной точке и фиксирует размеры области перемещения.
    /// </summary>
    /// <param name="startX">Начальная координата левого края спрайта.</param>
    /// <param name="startY">Начальная координата верхнего края спрайта.</param>
    /// <param name="clientWidth">Ширина клиентской области окна.</param>
    /// <param name="clientHeight">Высота клиентской области окна.</param>
    internal SpriteController(int startX, int startY, int clientWidth, int clientHeight)
    {
        _x = startX;
        _y = startY;
        _clientWidth = clientWidth;
        _clientHeight = clientHeight;
    }

    /// <summary>
    /// Сдвигает спрайт по направлению WASD, если новая позиция остаётся внутри окна.
    /// </summary>
    /// <param name="virtualKey">Виртуальный код клавиши из WM_KEYDOWN.</param>
    /// <returns>True, если позиция изменилась и требуется перерисовка.</returns>
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

    /// <summary>
    /// Обновляет границы после WM_SIZE, чтобы спрайт не оказался за пределами уменьшенного окна.
    /// </summary>
    /// <param name="clientWidth">Новая ширина клиентской области.</param>
    /// <param name="clientHeight">Новая высота клиентской области.</param>
    internal void UpdateClientSize(int clientWidth, int clientHeight)
    {
        _clientWidth = clientWidth;
        _clientHeight = clientHeight;

        if (_x + SpriteWidth > _clientWidth)
        {
            _x = Math.Max(0, _clientWidth - SpriteWidth);
        }

        if (_y + SpriteHeight > _clientHeight)
        {
            _y = Math.Max(0, _clientHeight - SpriteHeight);
        }
    }

    /// <summary>Возвращает текущую X-координату левого края спрайта.</summary>
    internal int X => _x;

    /// <summary>Возвращает текущую Y-координату верхнего края спрайта.</summary>
    internal int Y => _y;

    // Сдвиг вверх допустим только если верхний край не пересечёт границу клиентской области
    private bool MoveUpIfWithinBounds()
    {
        if (_y - MoveStep < 0)
        {
            return false;
        }

        _y -= MoveStep;
        return true;
    }

    // Нижний край (y + высота) не должен выходить за clientHeight
    private bool MoveDownIfWithinBounds()
    {
        if (_y + MoveStep + SpriteHeight > _clientHeight)
        {
            return false;
        }

        _y += MoveStep;
        return true;
    }

    // Левый край не может стать отрицательным
    private bool MoveLeftIfWithinBounds()
    {
        if (_x - MoveStep < 0)
        {
            return false;
        }

        _x -= MoveStep;
        return true;
    }

    // Правый край (x + ширина) не должен выходить за clientWidth
    private bool MoveRightIfWithinBounds()
    {
        if (_x + MoveStep + SpriteWidth > _clientWidth)
        {
            return false;
        }

        _x += MoveStep;
        return true;
    }
}
