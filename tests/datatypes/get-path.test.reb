; datatypes/get-path.r
; minimum
; empty get-path test
[#1947
    (get-path? load "#[get-path! [[a] 1]]")
]
(
    all [
        get-path? a: load "#[get-path! [[a b c] 2]]"
        2 == index? a
    ]
)
