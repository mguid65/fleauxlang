# Oops! All Postfix Operators

```
(2, 1) -> +; // 3
(2, 1) -> -; // 1
(2, 1) -> *; // 2
(2, 1) -> /; // 2
```

# Oops! Significant parentheses that look redundant...

```
(2, 1) -> Std.Println; // 2 1
((2, 1)) -> Std.Println; // (2, 1)
```
The first one is piping 2 arguments 2 and 1 into Std.Println. 
The second on is piping a single argument into Std.Println, that being the tuple `(2, 1)`.
