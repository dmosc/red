# red
Lightweight UNIX text editor for CLI.

## Commands

**Compile editor:**
```
make red
```

**Open a document:**
> Omit the `file_name` argument to create a new document.
```
./red [file_name]
```

### Terminal states
**Read mode:**
In this mode you can navigate through the document without having to worry about editing
the document by accident.
> It is the initial state of the editor as well.
```
ctrl + r
```

**Edit mode:** This mode allows you to edit the document in any way you want. It is possible
to insert/remove characters.
```
ctrl + e
```

**Command mode:** A prompt at the bottom of the editor will show, allowing you to insert any
valid command to perform special actions.
```
ctrl + c
```

**Exit editor:** Any unsaved change will not persist.
```
ctrl + q
```

### Command mode operations
This commands naturally require the terminal to be in `command` mode. Each command can be called
in a few different ways. They're separated by a `|` character.

**Save a document**
> A prompt to set a name will show when working with a new document.
```
[save | s]
```

**Jump to line**
> Move the cursor to a specific line with blazing speed! 🔥
```
[line | l | n] [line_number]
```

**Find search**
> This will reposition the cursor to the first incidence that matches your search and tell you how
> many times the search repeats within the document.
> The search is enhanced with POSIX regular expressions: 
> https://en.wikibooks.org/wiki/Regular_Expressions/POSIX_Basic_Regular_Expressions
```
[find | f] [search]
```