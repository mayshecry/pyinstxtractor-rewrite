def greet(name):
    return f"Hello, {name}! You are running PyInstaller {__import__('sys').version}"

if __name__ == "__main__":
    print(greet("world"))
    print("This is a test entry point.")
