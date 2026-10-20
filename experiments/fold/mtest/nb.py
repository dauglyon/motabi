import marimo
app = marimo.App()

@app.cell
def _():
    import math
    return (math,)

@app.cell
def _(math):
    radius = 2.0
    area = math.pi * radius ** 2
    return area, radius

@app.cell
def _(area):
    print(f"area={area:.3f}")
    return

if __name__ == "__main__":
    app.run()
