# HKRPC

�ṩԶ�̽���hook�Ƚӿ�

# ��������

1. ע��HRRPC.dll��Ŀ�����
2. ����NamedPipe�� ``` \\.\pipe\hkrpc\<Ŀ�������> ```
3. ͨ��NamedPipeͨ��


# ��Ϣ����ʽ
```
packet{
    uint16 magic = 0xF8FB;
    uint16 jsonlen;
    char json[jsonlen];
}
```

# �ӿ�˵��
���� --> ��ʾ�ͻ��˷�������ˣ� <-- ��ʾ����˷����ͻ���

### ��������

- ����������ֵ
- ���أ�ͬ����ֵ
```
--> { "method": "echo", "params": ["hello","world"], "id": "1"}
<-- { "result": ["hello","world"], "id": "1"}
```

### ����Ŀ�꺯������

- ������ģ������������|������ַ���������ͱ�����ջƫ��(��ѡ)
- ���أ�hook id
```
--> { "method": "hook", "params": ["user32.dll", "MessageBoxA", ["intptr","string","string","int"],[4,8,12,16]], "id": "OnMessageBox"} 
<-- { "result": 321321, "id": "OnMessageBox"}  
```
 
### ֪ͨ���������� 

- ���������øú����Ĳ���ֵ
```
<-- { "method": "hook_notify", "params": [642618,"hello","world",0], "requestid":"OnMessageBox"} 
```

### ȡ����������

- ������hook id
```
--> { "method": "hook_delete", "params": [321321], "id": "111"} 
<-- { "result": "ok", "id": "111"}  
```

### ����Ŀ�꺯��

- ������ģ������������|������ַ������Լ�����������������ͱ�(��ѡ)����������(��ѡ)
- ���أ����øú����ķ���ֵ
```
--> { "method": "call", "params": ["user32.dll", "MessageBoxA","stdcall",[null,"hello","world",0], ["intptr","string","string","int"],"int"], "id": "111"} 
<-- { "result": 0, "id": "111"} 
```

### ��ȡģ��汾

- ������ģ����
- ���أ��汾���ַ���
```
--> { "method": "module_version", "params": ["user32.dll"], "id": "111"} 
<-- { "result": "6.0.1.1600", "id": "111"}  
```






