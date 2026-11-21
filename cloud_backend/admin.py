"""Trusted local provisioning CLI; no unauthenticated admin HTTP endpoints."""
import argparse
import json

from sqlalchemy.exc import IntegrityError

from .config import Settings
from .database import Database, devices, readers
from .service import provision_device, provision_tenant


def main():
    parser = argparse.ArgumentParser(description='接收服务本地管理工具；凭据只在创建时显示')
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('init-db', help='初始化独立接收数据库')
    tenant = sub.add_parser('create-tenant', help='登记单位并生成单位只读查询凭据')
    tenant.add_argument('--id', required=True)
    tenant.add_argument('--name', required=True)
    device = sub.add_parser('register-device', help='登记真实设备并生成独立上传凭据')
    device.add_argument('--tenant', required=True)
    device.add_argument('--id', required=True)
    device.add_argument('--name', required=True)
    revoke = sub.add_parser('disable-device', help='禁用上传凭据，保留历史数据')
    revoke.add_argument('--id', required=True)
    revoke_reader = sub.add_parser('disable-readers', help='撤销某单位全部查询凭据')
    revoke_reader.add_argument('--tenant', required=True)
    args = parser.parse_args()
    db = Database(Settings.from_env().database_url)
    try:
        db.initialize()
        if args.command == 'init-db':
            result = {'initialized': True}
        elif args.command == 'create-tenant':
            result = {'tenantId': args.id, 'queryToken': provision_tenant(db, args.id, args.name)}
        elif args.command == 'register-device':
            result = {'tenantId': args.tenant, 'deviceId': args.id,
                      'deviceToken': provision_device(db, args.tenant, args.id, args.name)}
        elif args.command == 'disable-device':
            with db.engine.begin() as connection:
                changed = connection.execute(devices.update().where(devices.c.id == args.id).values(enabled=False))
                result = {'disabled': changed.rowcount}
        else:
            with db.engine.begin() as connection:
                changed = connection.execute(readers.update().where(readers.c.tenant_id == args.tenant).values(enabled=False))
                result = {'disabled': changed.rowcount}
        print(json.dumps(result, ensure_ascii=False, indent=2))
    except IntegrityError:
        parser.exit(1, '登记失败：编号已存在或关联单位无效；未覆盖原登记。\n')
    except ValueError as exc:
        parser.exit(1, f'登记失败：{exc}\n')
    finally:
        db.engine.dispose()


if __name__ == '__main__':
    main()
