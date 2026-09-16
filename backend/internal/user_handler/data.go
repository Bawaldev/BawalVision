package user_handler

import (
	"time"

	"gorm.io/gorm"
)

type HashKey struct {
	Key []byte `gorm:"column:hash_key;type:bytea"`
}

type BlockKey struct {
	Key []byte `gorm:"column:block_key;type:bytea"`
}

type ProtectiveKey struct {
	Hashkey  HashKey  `gorm:"embedded"`
	Blockkey BlockKey `gorm:"embedded"`
}

type User struct {
	ID           string        `json:"id" gorm:"type:uuid;primaryKey;default:gen_random_uuid()"`
	Name         string        `json:"name" gorm:"column:name;type:varchar(100);not null"`
	Email        string        `json:"email" gorm:"column:email;type:varchar(100);uniqueIndex;not null"`
	Password     string        `json:"-" gorm:"column:password_hash;type:varchar(255);not null"`
	Securecookie ProtectiveKey `json:"-" gorm:"embedded"`

	CreatedAt time.Time      `json:"created_at" gorm:"column:created_at;type:timestamptz;not null;default:now()"`
	UpdatedAt time.Time      `json:"updated_at" gorm:"column:updated_at;type:timestamptz;not null;default:now()"`
	DeletedAt gorm.DeletedAt `json:"-" gorm:"index"`
}

type UserTelemetry struct {
	UserID    string    `json:"user_id"`
	Email     string    `json:"email"`
	Event     string    `json:"event"` // "user_login","cookie_rotated","profile_updated"
	Timestamp time.Time `json:"timestamp"`
	IPAddress string    `json:"ip_address"`
	UserAgent string    `json:"user_agent"`
}

type RegisterRequest struct {
	Name     string `json:"name"`
	Email    string `json:"email"`
	Password string `json:"password"`
}

type LoginRequest struct {
	Email    string `json:"email"`
	Password string `json:"password"`
}
