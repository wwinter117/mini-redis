# mini-redis
简易版本的redis(Redis 2.0)


## C-S通信协议

[Redis-RESP 官方文档](https://redis.io/docs/latest/develop/reference/protocol-spec/)

RESP（REdis Serialization Protocol）是Redis使用的一种序列化协议，用于客户端和服务器之间的通信。RESP协议设计简单高效，支持以下几种类型的数据：

+ 简单字符串（Simple Strings）
+ 错误（Errors）
+ 整数（Integers）
+ 批量字符串（Bulk Strings）
+ 数组（Arrays）

### RESP 数据类型格式

**1. 简单字符串（Simple Strings）**

简单字符串以 + 开头，后跟字符串本身，最后以 \r\n 结尾。

示例：
```text
+OK\r\n
```

**2. 错误（Errors）**

错误消息以 - 开头，后跟错误信息，最后以 \r\n 结尾。

示例：
```text
-ERR unknown command 'foobar'\r\n
```

**3. 整数（Integers）**

整数以 : 开头，后跟整数值，最后以 \r\n 结尾。

示例：
```text
:1000\r\n
```


**4. 批量字符串（Bulk Strings）**

批量字符串以 $ 开头，后跟字符串的长度，最后以 \r\n 结尾。然后是字符串内容，再次以 \r\n 结尾。如果长度为-1，表示空值。

示例：

```text
$6\r\nfoobar\r\n
$-1\r\n
```

**5. 数组（Arrays）**

数组以 * 开头，后跟元素数量，最后以 \r\n 结尾。然后是数组中各元素的 RESP 表示。如果数量为-1，表示空数组。