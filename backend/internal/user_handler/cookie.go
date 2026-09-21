package user_handler

import (
	"errors"
	"os"
	"time"

	"github.com/gofiber/fiber/v2"
	securecookie "github.com/gorilla/securecookie"
)

type SessionManager struct {
	cookie *securecookie.SecureCookie
}

var (
	// Global Session manager instance
	Session = NewSessionManager()
)

// NewSessionManager creates a thread-safe server-level session manager
func NewSessionManager() *SessionManager {
	hashKey := []byte(os.Getenv("SESSION_HASH_KEY"))
	if len(hashKey) == 0 {
		hashKey = securecookie.GenerateRandomKey(64) // HMAC-SHA512
	}

	blockKey := []byte(os.Getenv("SESSION_BLOCK_KEY"))
	if len(blockKey) == 0 {
		blockKey = securecookie.GenerateRandomKey(32) // AES-256
	}

	return &SessionManager{
		cookie: securecookie.New(hashKey, blockKey),
	}
}

// SetSessionCookie encodes and encrypts userID into a Fiber HTTP-only cookie
func (s *SessionManager) SetSessionCookie(c *fiber.Ctx, userID string, duration time.Duration) error {
	encoded, err := s.cookie.Encode("session", userID)
	if err != nil {
		return err
	}

	c.Cookie(&fiber.Cookie{
		Name:     "session",
		Value:    encoded,
		Path:     "/",
		HTTPOnly: true,
		Secure:   false, // Set to true in production HTTPS
		SameSite: "Lax",
		Expires:  time.Now().Add(duration),
	})

	return nil
}

// GetSessionUserID decodes and decrypts the session cookie returning the authentic userID
func (s *SessionManager) GetSessionUserID(c *fiber.Ctx) (string, error) {
	cookieVal := c.Cookies("session")
	if cookieVal == "" {
		return "", errors.New("session cookie not found")
	}

	var userID string
	if err := s.cookie.Decode("session", cookieVal, &userID); err != nil {
		return "", errors.New("invalid or expired session cookie")
	}

	return userID, nil
}
