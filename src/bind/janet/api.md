# persimmon API

## persimmon

[assoc](#assoc), [assoc!](#assoc-1), [conj](#conj), [conj!](#conj-1), [disj](#disj), [disj!](#disj-1), [dissoc](#dissoc), [dissoc!](#dissoc-1), [first](#first), [has-key?](#has-key), [into](#into), [list](#list), [map](#map), [persistent!](#persistent), [rest](#rest), [set](#set), [to-array](#to-array), [to-table](#to-table), [transient](#transient), [vec](#vec)

## assoc

**cfunction**  | [source][1]

```janet
(assoc coll key value)
```

Returns a new persistent vector or map with key associated with value. Vector keys are indices and may be negative. Associating nil in a map removes the key. coll is unchanged.

[1]: wrapper.c#L1581


## assoc!

**cfunction**  | [source][2]

```janet
(assoc! trans key value)
```

Associates key with value in a vector or map transient in place. Vector keys are indices and may be negative. Associating nil in a map removes the key. Returns trans.

[2]: wrapper.c#L1738


## conj

**cfunction**  | [source][3]

```janet
(conj coll x)
```

Returns a new persistent collection with x added: at the end of a vector, at the front of a list, or as an element of a set. coll is unchanged.

[3]: wrapper.c#L1549


## conj!

**cfunction**  | [source][4]

```janet
(conj! trans x)
```

Adds x to a vector or set transient in place. Returns trans.

[4]: wrapper.c#L1715


## disj

**cfunction**  | [source][5]

```janet
(disj set x)
```

Returns a new persistent set without x. set is unchanged.

[5]: wrapper.c#L1632


## disj!

**cfunction**  | [source][6]

```janet
(disj! trans x)
```

Removes x from a set transient in place. Returns trans.

[6]: wrapper.c#L1778


## dissoc

**cfunction**  | [source][7]

```janet
(dissoc map key)
```

Returns a new persistent map without key. map is unchanged.

[7]: wrapper.c#L1618


## dissoc!

**cfunction**  | [source][8]

```janet
(dissoc! trans key)
```

Removes key from a map transient in place. Returns trans.

[8]: wrapper.c#L1767


## first

**cfunction**  | [source][9]

```janet
(first list)
```

Returns the first element of a persistent list, or nil if the list is empty.

[9]: wrapper.c#L1828


## has-key?

**cfunction**  | [source][10]

```janet
(has-key? coll key)
```

Returns true if the persistent map or set coll contains key, or false otherwise. A nil key always returns false.

[10]: wrapper.c#L1794


## into

**cfunction**  | [source][11]

```janet
(into target coll)
```

Returns a new persistent collection of target's kind holding target's elements and those of coll. A map takes coll's entries, or its elements when each is a key-value pair. Elements go at the end of a vector and in front of a list. target is unchanged.

[11]: wrapper.c#L1511


## list

**cfunction**  | [source][12]

```janet
(list & xs)
```

Creates a persistent list whose elements are xs, in order. Splice a collection to convert one. Returns the list.

[12]: wrapper.c#L1479


## map

**cfunction**  | [source][13]

```janet
(map & kvs)
```

Creates a persistent map from alternating keys and values. A key whose value is nil adds no entry. Use into to convert a dictionary. Returns the map.

[13]: wrapper.c#L1487


## persistent!

**cfunction**  | [source][14]

```janet
(persistent! trans)
```

Consumes a vector, map or set transient and returns its persistent collection. Using trans afterwards is an error.

[14]: wrapper.c#L1683


## rest

**cfunction**  | [source][15]

```janet
(rest list)
```

Returns a persistent list without its first element. The rest of an empty list is an empty list. list is unchanged.

[15]: wrapper.c#L1847


## set

**cfunction**  | [source][16]

```janet
(set & xs)
```

Creates a persistent set whose elements are xs. Nil cannot be an element. Splice a collection to convert one. Returns the set.

[16]: wrapper.c#L1496


## to-array

**cfunction**  | [source][17]

```janet
(to-array coll)
```

Copies a persistent collection into a mutable Janet array. Map entries become key-value tuples. Returns the array.

[17]: wrapper.c#L1867


## to-table

**cfunction**  | [source][18]

```janet
(to-table map)
```

Copies the entries of a persistent map into a mutable Janet table. Returns the table.

[18]: wrapper.c#L1815


## transient

**cfunction**  | [source][19]

```janet
(transient coll)
```

Creates a mutable, uniquely owned transient from a persistent vector, map or set. coll is unchanged. Returns the transient.

[19]: wrapper.c#L1647


## vec

**cfunction**  | [source][20]

```janet
(vec & xs)
```

Creates a persistent vector whose elements are xs, in order. Splice a collection or use into to convert one. Returns the vector.

[20]: wrapper.c#L1472

