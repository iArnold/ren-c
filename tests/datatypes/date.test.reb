; datatypes/date.r
(date? 25/Sep/2006)
(not date? 1)
(date! = type of 25/Sep/2006)

; alternative formats
(25/Sep/2006 = 25/9/2006)
(25/Sep/2006 = 25-Sep-2006)
(25/Sep/2006 = 25-9-2006)
(25/Sep/2006 = make date! "25/Sep/2006")
(25/Sep/2006 = to date! "25-Sep-2006")
("25-Sep-2006" = mold 25/Sep/2006)

; minimum
(date? 1/Jan/0000)

; another minimum
(date? 1/Jan/0000/0:00)

; extreme behaviour
(
    did any [
        error? trap [date-d: 1/Jan/0000 - 1]
        date-d = load mold date-d
    ]
)
(
    did any [
        error? trap [date-d: 31-Dec-16383 + 1]
        date-d = load mold date-d
    ]
)

[#1250 (
    did all [
        error? trap [load "1/11/-00"]
        error? trap [load "1/11/-0"]
        load "1/11/0" = load "1/11/00"
    ]
)]
