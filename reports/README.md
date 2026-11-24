# Báo cáo tiến độ đề tài chia sẻ tệp P2P

Cập nhật ngày 24/11/2026 cho nhánh `feature/client-core`, tại bản sửa `26983ef`.

Hệ thống viết bằng C11, sử dụng IPv4/TCP và pthread, với Ubuntu 20.04 là môi trường đích. Máy chủ quản lý tài khoản, phiên đăng nhập và thông tin tìm kiếm. Nội dung tệp đi trực tiếp giữa hai máy khách. [Đặc tả giao thức](../docs/PROTOCOL.md) · [Hướng dẫn chạy](../README.md)

## 1. Tiến độ theo phân công

### Cao Trường Xuân (20236016) · Máy chủ và mạng chung

| Hạng mục | Kết quả trên nhánh hiện tại |
| --- | --- |
| TCP và xử lý thông điệp | Máy chủ đa luồng nhận lệnh theo dòng CRLF, xử lý cả khi một dòng bị chia qua nhiều lần đọc hoặc nhiều dòng đến cùng lúc. |
| Tài khoản và phiên | Đăng ký lưu tài khoản với mật khẩu băm PBKDF2-HMAC-SHA256 có salt. Đăng nhập xác thực kết nối hiện tại. `HELLO` ghi ID, IPv4 nguồn và cổng P2P. Đăng xuất hoặc ngắt kết nối giải phóng ID và quảng bá của phiên. |
| Danh sách chia sẻ và tìm kiếm | Máy chủ thêm, cập nhật và gỡ quảng bá theo phiên. Tìm kiếm khớp toàn bộ tên, phân biệt hoa và thường, đồng thời trả mọi peer phù hợp theo thứ tự ID. |
| Nhật ký | Ghi thời gian UTC, vòng đời kết nối và thao tác tài khoản hoặc chia sẻ. Mật khẩu và đầu vào lỗi thô được loại khỏi log. |

### Nguyễn Đức Thắng (20235998) · Client và truyền tệp P2P

| Hạng mục chấm | Điểm | Kết quả trên nhánh hiện tại |
| --- | ---: | --- |
| Giao tiếp qua socket | 2 | CLI giao tiếp với máy chủ và peer qua TCP có bộ đệm. Bốn worker nhận yêu cầu tải lên đồng thời. |
| Công bố ID và cổng P2P | 1 | Client giữ ID 32 bit trong `config.txt`. Sau đăng nhập, client gửi `HELLO` kèm cổng đang lắng nghe và duy trì phiên với chu kỳ 10 giây. |
| Đăng ký chia sẻ | 2 | Client lưu tên và đường dẫn vào `index.txt` trước khi công bố. `shares` hiển thị `ANNOUNCED` sau khi máy chủ xác nhận và `PENDING` khi mục chưa được công bố. |
| Hủy chia sẻ | 2 | Client lưu việc xóa trước, sau đó gỡ quảng bá trên máy chủ. Lỗi ghi catalog giữ nguyên trạng thái chia sẻ. Client thử lại nếu yêu cầu gỡ bị từ chối và phiên còn hoạt động. |
| Tải tệp trực tiếp | 3 | Client chọn peer từ kết quả tìm kiếm, nhận tệp nhị phân qua các khung có độ dài và chỉ tạo tên đích sau khi nhận đủ khung cuối. |

## 2. Thiết kế giao thức trên nhánh hiện tại

### Kết nối điều khiển với máy chủ

Lệnh, dòng kết quả và phản hồi dùng CRLF (`\r\n`). Lệnh phân biệt hoa và thường, các trường cách nhau đúng một dấu cách. Tên tệp là tên cơ sở ASCII in được, mã hóa phần cần thoát bằng `%HH` viết hoa. Ví dụ `bao cao.txt` thành `bao%20cao.txt`.

| Lệnh trên đường truyền | Phản hồi thành công và tác dụng |
| --- | --- |
| `REGISTER <username> <password>` | `201`. Tạo tài khoản. |
| `LOGIN <username> <password>` | `200`. Xác thực kết nối. Client tiếp tục gửi `HELLO`. |
| `HELLO <client_id> <port>` | `200`. Máy chủ ghép cổng được khai báo với IPv4 của kết nối. |
| `SHARE <filename> <size>` | `200`. Thêm hoặc cập nhật tên và kích thước cho phiên đã đăng nhập và gửi `HELLO`. |
| `SEARCH <filename>` | Mỗi kết quả có dạng `FILE <filename> <size> <client_id> <IPv4> <port>`. Dòng `200 END` kết thúc cả tìm kiếm có kết quả lẫn tìm kiếm rỗng. |
| `UNSHARE <filename>` | `200` nếu đã gỡ mục của phiên gửi. Client cũng coi `404` là xác nhận mục đó đã vắng mặt. |
| `LOGOUT` | `200`. Giải phóng quảng bá và ID. Kết nối vẫn có thể dùng để đăng nhập lại. |

Client chỉ cho chọn kết quả tải sau khi đọc đủ phản hồi tìm kiếm đến `200 END`. CLI hiển thị số thứ tự, tên, ID, IPv4 và cổng. Kích thước trong dòng `FILE` là số byte tại thời điểm công bố.

### Kết nối truyền tệp giữa hai peer

Bên tải mở TCP tới IPv4 và cổng đã chọn rồi gửi `GET <filename>\r\n`. Bên cung cấp kiểm tra catalog, mở tệp và trả `200 CHUNKED\r\n`. Mỗi header khung ghi số byte payload ngay sau nó. Giới hạn mỗi khung là 65.536 byte.

| Header khung | Dữ liệu theo sau |
| --- | --- |
| `<data> <length>\r\n` | Đúng `length` byte, với `length` dương. Sau payload là khung tiếp theo. |
| `<end> <length>\r\n` | Đúng `length` byte cuối. Tệp rỗng dùng `<end> 0\r\n`. |

Ví dụ chuỗi sau truyền năm byte `abcde`.

```text
200 CHUNKED\r\n<data> 3\r\nabc<end> 2\r\nde
```

Bên cung cấp đọc tệp nguồn đến EOF thực tế, nên phần nối thêm trước EOF có thể được truyền và thao tác cắt tệp có thể làm EOF đến sớm. Bên tải kết thúc khi đọc đủ payload của `<end>`. Nếu kết nối đóng trước đó hoặc khung sai cú pháp, bên tải dọn tệp tạm. Tệp tải thành công được đồng bộ và công bố mà không ghi đè đường dẫn sẵn có.

### Vòng đời chia sẻ và phục hồi lỗi

- `index.txt` lưu tên và đường dẫn tuyệt đối dưới dạng hex, có tệp khóa riêng. Client đăng lại các mục còn đọc được khi đăng nhập hoặc chạy `hello` thủ công.
- `unshare` lưu catalog bằng thay thế nguyên tử trước khi gửi lệnh lên máy chủ. Nếu lưu lỗi, quảng bá và quyền phục vụ cũ vẫn còn. Khi lưu thành công, yêu cầu tải mới bị chặn và lượt tải đã mở tệp có thể tiếp tục.
- Client giữ yêu cầu gỡ bị từ chối trong RAM để thử lại ở chu kỳ keepalive hoặc khi `hello` thủ công thành công. Các lệnh đi qua cùng khóa điều khiển. Một `SHARE` mới thành công cho cùng tên thay thế quảng bá và hủy yêu cầu gỡ cũ. Nếu kết nối mất trước phản hồi, máy chủ dọn quảng bá của phiên và lần khởi động sau đọc catalog đã xóa mục.
- Khi peer không mở được nguồn, nó trả `404`. Lỗi tải hiển thị cục bộ. Người chia sẻ có thể dùng `hello` để làm mới danh sách hoặc `unshare` để gỡ mục.

### Trạng thái đối chiếu với `main`

Commit `267fbf0` trên `origin/main` đưa vào `SHARE <client_id> <filename> <size>`, kết quả tìm kiếm `200-...` có revision, phản hồi `200 OK` và `REPORT` từ bên tải. Bản trên `main` dùng header truyền tệp `200 <size>`. Checklist đề xuất bổ sung phản hồi `200 <revision>` cho `SHARE` và báo cáo từ bên cung cấp dựa trên thiết kế đó. Trong đợt này, nhánh `feature/client-core` dùng các định dạng điều khiển và khung dữ liệu ở trên theo quyết định giữ giao thức hiện tại.

## 3. Kết quả kiểm thử

- `make test` trên Docker Ubuntu 20.04 chạy thành công cả 3 bộ test C và 86 test Python, gồm ca từ chối quyền truy cập kho tài khoản.
- Kiểm thử hồi quy `unshare` xác nhận bốn tình huống. Lỗi ghi catalog giữ cả hai phía, client tự thử lại khi server từ chối, chia sẻ lại cùng tên giữ quảng bá mới, và mất câu trả lời không làm mục đã xóa xuất hiện lại sau khởi động.
- LAN mô phỏng dùng ba container Ubuntu 20.04 với IPv4 riêng, một máy chủ và hai client có tài khoản tách biệt. Client tìm đúng IPv4 và cổng peer, tải tệp 1.048.832 byte khớp từng byte, rồi xác nhận mỗi lần `unshare` chỉ gỡ quảng bá của peer đã gửi lệnh.

## 4. Hạng mục nghiệm thu còn lại

- Chạy cùng quy trình trên hai máy trạm Ubuntu 20.04 qua mạng LAN thực tế.
- Kiểm tra các tình huống gián đoạn đã nêu ở báo cáo trước. Gồm mất kết nối điều khiển khi đang nhập mật khẩu, Ctrl+C đúng lúc đồng bộ và công bố tệp đích, và mất kết nối điều khiển khi lượt tải đang chờ dữ liệu từ peer.
