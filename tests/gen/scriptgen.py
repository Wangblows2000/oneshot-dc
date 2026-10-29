"""Tiny helper for writing host_runner scripts with automatic frame numbers."""
class Script:
    def __init__(self, header):
        self.out = [f"# {l}" for l in header]
        self.t = 0
    def emit(self, s, dt=0):
        self.out.append(f"{self.t} {s}"); self.t += dt
    def comment(self, s): self.out.append(f"# --- {s}")
    def press(self, btn, gap=10):
        self.emit(btn, 2); self.emit("-", gap)
    def wait(self, n): self.t += n
    def boot_to_map(self):
        """title -> first question -> wake-up sequence (as in opening.txt)."""
        self.emit("-", 30); self.press("A", 128); self.t = 200; self.emit("advance 3000", 10)
    def select_item(self, index):
        """B: menu, A: Items, DOWN x index, A: select (the game's 'activate item')."""
        self.press("B", 14); self.press("A", 12)
        for _ in range(index): self.press("DOWN", 6)
        self.press("A", 20)
    def hold_item(self, inventory, item):
        """select <item> given the sorted list of ids currently in the inventory."""
        self.select_item(sorted(inventory).index(item))
    def advance(self, n=3000): self.emit(f"advance {n}", 8)
    def save(self, path):
        open(path, 'w').write('\n'.join(self.out) + '\n')
