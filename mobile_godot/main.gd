extends Node2D

var stars: Array[Vector2] = []
var rng := RandomNumberGenerator.new()

func _ready() -> void:
    rng.seed = 3500
    for i in range(180):
        stars.append(Vector2(rng.randf_range(0.0, 1280.0), rng.randf_range(0.0, 720.0)))
    queue_redraw()

func _draw() -> void:
    draw_rect(Rect2(0, 0, 1280, 720), Color("#050817"))
    for star in stars:
        var radius := rng.randf_range(0.6, 1.8)
        draw_circle(star, radius, Color(0.65, 0.82, 1.0, 0.8))
    draw_circle(Vector2(980, 170), 92.0, Color("#182f62"))
    draw_circle(Vector2(980, 170), 78.0, Color("#214a83"))
    draw_string(ThemeDB.fallback_font, Vector2(64, 92), "ДЕТИ ЭЛЬТАН", HORIZONTAL_ALIGNMENT_LEFT, -1, 42, Color("#8fe8ff"))
    draw_string(ThemeDB.fallback_font, Vector2(68, 132), "MOBILE // FIRST CONTACT", HORIZONTAL_ALIGNMENT_LEFT, -1, 18, Color("#88a7c9"))
    draw_string(ThemeDB.fallback_font, Vector2(68, 590), "Вертикальный срез CE-P00: событие → выбор → сохранение", HORIZONTAL_ALIGNMENT_LEFT, -1, 20, Color("#d4e8ff"))
