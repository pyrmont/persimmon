# persimmon API

## persimmon

[assoc](#assoc), [assoc!](#assoc-1), [conj](#conj), [conj!](#conj-1), [disj](#disj), [disj!](#disj-1), [dissoc](#dissoc), [dissoc!](#dissoc-1), [first](#first), [has-key?](#has-key), [list](#list), [map](#map), [persistent!](#persistent), [rest](#rest), [set](#set), [to-array](#to-array), [to-table](#to-table), [transient](#transient), [vec](#vec)

## assoc

**cfunction**  | [source][1]

```janet
(assoc coll key value)
```

Returns a new persistent vector or map with key associated with value. Vector keys are indices and may be negative. Associating nil in a map removes the key. coll is unchanged.

[1]: wrapper.c#L1369


## assoc!

**cfunction**  | [source][2]

```janet
(assoc! trans key value)
```

Associates key with value in a vector or map transient in place. Vector keys are indices and may be negative. Associating nil in a map removes the key. Returns trans.

[2]: wrapper.c#L1526


## conj

**cfunction**  | [source][3]

```janet
(conj coll x)
```

Returns a new persistent collection with x added: at the end of a vector, at the front of a list, or as an element of a set. coll is unchanged.

[3]: wrapper.c#L1337


## conj!

**cfunction**  | [source][4]

```janet
(conj! trans x)
```

Adds x to a vector or set transient in place. Returns trans.

[4]: wrapper.c#L1503


## disj

**cfunction**  | [source][5]

```janet
(disj set x)
```

Returns a new persistent set without x. set is unchanged.

[5]: wrapper.c#L1420


## disj!

**cfunction**  | [source][6]

```janet
(disj! trans x)
```

Removes x from a set transient in place. Returns trans.

[6]: wrapper.c#L1566


## dissoc

**cfunction**  | [source][7]

```janet
(dissoc map key)
```

Returns a new persistent map without key. map is unchanged.

[7]: wrapper.c#L1406


## dissoc!

**cfunction**  | [source][8]

```janet
(dissoc! trans key)
```

Removes key from a map transient in place. Returns trans.

[8]: wrapper.c#L1555


## first

**cfunction**  | [source][9]

```janet
(first list)
```

Returns the first element of a persistent list, or nil if the list is empty.

[9]: wrapper.c#L1616


## has-key?

**cfunction**  | [source][10]

```janet
(has-key? coll key)
```

Returns true if the persistent map or set coll contains key, or false otherwise. A nil key always returns false.

[10]: wrapper.c#L1582


## list

**cfunction**  | [source][11]

```janet
(list &opt coll)
```

Creates a persistent list, optionally populated from the indexed collection coll in the same order. Returns the list.

[11]: wrapper.c#L1245


## map

**cfunction**  | [source][12]

```janet
(map &opt dict)
```

Creates a persistent map, optionally populated from the dictionary dict. Entries whose value is nil are omitted. Returns the map.

[12]: wrapper.c#L1266


## persistent!

**cfunction**  | [source][13]

```janet
(persistent! trans)
```

Consumes a vector, map or set transient and returns its persistent collection. Using trans afterwards is an error.

[13]: wrapper.c#L1471


## rest

**cfunction**  | [source][14]

```janet
(rest list)
```

Returns a persistent list without its first element. The rest of an empty list is an empty list. list is unchanged.

[14]: wrapper.c#L1635


## set

**cfunction**  | [source][15]

```janet
(set &opt coll)
```

Creates a persistent set, optionally populated from the indexed collection coll. Nil cannot be an element. Returns the set.

[15]: wrapper.c#L1301


## to-array

**cfunction**  | [source][16]

```janet
(to-array coll)
```

Copies a persistent collection into a mutable Janet array. Map entries become key-value tuples. Returns the array.

[16]: wrapper.c#L1655


## to-table

**cfunction**  | [source][17]

```janet
(to-table map)
```

Copies the entries of a persistent map into a mutable Janet table. Returns the table.

[17]: wrapper.c#L1603


## transient

**cfunction**  | [source][18]

```janet
(transient coll)
```

Creates a mutable, uniquely owned transient from a persistent vector, map or set. coll is unchanged. Returns the transient.

[18]: wrapper.c#L1435


## vec

**cfunction**  | [source][19]

```janet
(vec &opt coll)
```

Creates a persistent vector, optionally populated from the indexed collection coll. Returns the vector.

[19]: wrapper.c#L1215

