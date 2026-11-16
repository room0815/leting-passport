#pragma once

// 乐听护照：api.tenclass.net 服务器实际发出来的证书链（中间 CA + 交叉签名根）。
//
// 为什么不用默认的 esp_crt_bundle：
//   系统 bundle 里存在一张同名的 "DigiCert Global Root G2"（自签名版，
//   issuer 也是它自己），而服务器发的是交叉签名版（issuer 是
//   DigiCert Global Root CA），两者公钥不同。mbedtls 按 subject 名字找父证书，
//   会命中 bundle 里那张错的，于是报
//       "Certificate matched but signature verification failed"
//   TLS 握手直接失败 —— 现象就是「设备连不上小智、叫它不理人」。
//
// 用服务器实际证书链做显式信任锚可以绕开名字歧义。服务器换链时用
//   openssl s_client -connect api.tenclass.net:443 -showcerts
// 重新取第 2、3 张 PEM 覆盖这里即可。

static const char LETING_CA_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIEszCCA5ugAwIBAgIQCyWUIs7ZgSoVoE6ZUooO+jANBgkqhkiG9w0BAQsFADBh\n"
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
    "MjAeFw0xNzExMDIxMjI0MzNaFw0yNzExMDIxMjI0MzNaMGAxCzAJBgNVBAYTAlVT\n"
    "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
    "b20xHzAdBgNVBAMTFlJhcGlkU1NMIFRMUyBSU0EgQ0EgRzEwggEiMA0GCSqGSIb3\n"
    "DQEBAQUAA4IBDwAwggEKAoIBAQC/uVklRBI1FuJdUEkFCuDL/I3aJQiaZ6aibRHj\n"
    "ap/ap9zy1aYNrphe7YcaNwMoPsZvXDR+hNJOo9gbgOYVTPq8gXc84I75YKOHiVA4\n"
    "NrJJQZ6p2sJQyqx60HkEIjzIN+1LQLfXTlpuznToOa1hyTD0yyitFyOYwURM+/CI\n"
    "8FNFMpBhw22hpeAQkOOLmsqT5QZJYeik7qlvn8gfD+XdDnk3kkuuu0eG+vuyrSGr\n"
    "5uX5LRhFWlv1zFQDch/EKmd163m6z/ycx/qLa9zyvILc7cQpb+k7TLra9WE17YPS\n"
    "n9ANjG+ECo9PDW3N9lwhKQCNvw1gGoguyCQu7HE7BnW8eSSFAgMBAAGjggFmMIIB\n"
    "YjAdBgNVHQ4EFgQUDNtsgkkPSmcKuBTuesRIUojrVjgwHwYDVR0jBBgwFoAUTiJU\n"
    "IBiV5uNu5g/6+rkS7QYXjzkwDgYDVR0PAQH/BAQDAgGGMB0GA1UdJQQWMBQGCCsG\n"
    "AQUFBwMBBggrBgEFBQcDAjASBgNVHRMBAf8ECDAGAQH/AgEAMDQGCCsGAQUFBwEB\n"
    "BCgwJjAkBggrBgEFBQcwAYYYaHR0cDovL29jc3AuZGlnaWNlcnQuY29tMEIGA1Ud\n"
    "HwQ7MDkwN6A1oDOGMWh0dHA6Ly9jcmwzLmRpZ2ljZXJ0LmNvbS9EaWdpQ2VydEds\n"
    "b2JhbFJvb3RHMi5jcmwwYwYDVR0gBFwwWjA3BglghkgBhv1sAQEwKjAoBggrBgEF\n"
    "BQcCARYcaHR0cHM6Ly93d3cuZGlnaWNlcnQuY29tL0NQUzALBglghkgBhv1sAQIw\n"
    "CAYGZ4EMAQIBMAgGBmeBDAECAjANBgkqhkiG9w0BAQsFAAOCAQEAGUSlOb4K3Wtm\n"
    "SlbmE50UYBHXM0SKXPqHMzk6XQUpCheF/4qU8aOhajsyRQFDV1ih/uPIg7YHRtFi\n"
    "CTq4G+zb43X1T77nJgSOI9pq/TqCwtukZ7u9VLL3JAq3Wdy2moKLvvC8tVmRzkAe\n"
    "0xQCkRKIjbBG80MSyDX/R4uYgj6ZiNT/Zg6GI6RofgqgpDdssLc0XIRQEotxIZcK\n"
    "zP3pGJ9FCbMHmMLLyuBd+uCWvVcF2ogYAawufChS/PT61D9rqzPRS5I2uqa3tmIT\n"
    "44JhJgWhBnFMb7AGQkvNq9KNS9dd3GWc17H/dXa1enoxzWjE0hBdFjxPhUb0W3wi\n"
    "8o34/m8Fxw==\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIIEfjCCA2agAwIBAgIQD+Ayq4RNAzEGxQyOE8iwaDANBgkqhkiG9w0BAQsFADBh\n"
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBD\n"
    "QTAeFw0yNDAxMTgwMDAwMDBaFw0zMTExMDkyMzU5NTlaMGExCzAJBgNVBAYTAlVT\n"
    "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
    "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
    "2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
    "1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
    "q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
    "tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
    "vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo4IBMDCC\n"
    "ASwwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUTiJUIBiV5uNu5g/6+rkS7QYX\n"
    "jzkwHwYDVR0jBBgwFoAUA95QNVbRTLtm8KPiGxvDl7I90VUwDgYDVR0PAQH/BAQD\n"
    "AgGGMHQGCCsGAQUFBwEBBGgwZjAjBggrBgEFBQcwAYYXaHR0cDovL29jc3AuZGln\n"
    "aWNlcnQuY24wPwYIKwYBBQUHMAKGM2h0dHA6Ly9jYWNlcnRzLmRpZ2ljZXJ0LmNu\n"
    "L0RpZ2lDZXJ0R2xvYmFsUm9vdENBLmNydDBABgNVHR8EOTA3MDWgM6Axhi9odHRw\n"
    "Oi8vY3JsLmRpZ2ljZXJ0LmNuL0RpZ2lDZXJ0R2xvYmFsUm9vdENBLmNybDARBgNV\n"
    "HSAECjAIMAYGBFUdIAAwDQYJKoZIhvcNAQELBQADggEBAHRBl3jN7+XHBUK0dZnu\n"
    "hMdoNwD1nCROU3BTIh1TNzRI0bQ0m5+C/dCRzzlqoSAFHUlOi+OiDltWkXTzmQn6\n"
    "Z8bH5PFBy5sYpc/8cNPoSzhyqcpvvEZvv/Ivc0Up+dzma7vBDJC9WrMRUUlSFSQp\n"
    "kdXSmphDNkXJsgARmxzc18IN6LYMRiOWlY7RE2F900pPW60BvJHHNCX0bbSRj/Ql\n"
    "bmVq8wuftBD++D+RS8K++ujpMjFBROyWfBX+woQDGsMazkmgulQdnZrdj476elOL\n"
    "axRvrSgEorju1kJM7M65z2RUZrfzQYW/1rs8mRUXin6iEtad/Rv1ZI1WGYmWPyBm\n"
    "pbo=\n"
    "-----END CERTIFICATE-----\n";

static const unsigned int LETING_CA_PEM_LEN = sizeof(LETING_CA_PEM) - 1;
